#include "mdhardware.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "dsp56kEmu/dma.h"

// The block link of the Machinedrum, see mdblocklink.h for what it replaces and why.

namespace md
{
	namespace
	{
		constexpr dsp56k::TWord g_dmaEnable = 1u << dsp56k::DmaChannel::De;
		constexpr dsp56k::TWord g_dcrMask = 0x7fffff;				// a control register without its enable bit

		// The control words the two programs arm their link channels with
		constexpr dsp56k::TWord g_producerSend = 0x0e5a51;			// DMA 0: Y memory -> ESSI0 TX
		constexpr dsp56k::TWord g_producerReceive = 0x0e52c0;		// DMA 2: ESSI0 RX -> X memory
		constexpr dsp56k::TWord g_mixerSend = 0x0e5a50;				// DMA 2: X memory -> ESSI0 TX
		constexpr dsp56k::TWord g_mixerReceive = 0x0e52c4;			// DMA 4: ESSI0 RX -> Y memory

		// ESSI0 as a DMA request source (DSP56303 UM, DRS field)
		constexpr dsp56k::TWord g_requestEssi0Receive = 0b01010;
		constexpr dsp56k::TWord g_requestEssi0Transmit = 0b01011;

		constexpr uint64_t g_cyclesPerFrame = 2304;					// DSP cycles per codec frame, both DSPs

		// The first slot of a 96-cycle grid at or after a machine cycle
		uint64_t slotAtOrAfter(const uint64_t _cycle, const uint32_t _phase)
		{
			const uint64_t rest = (_cycle + BlockLink::SlotCycles - _phase) % BlockLink::SlotCycles;
			return rest ? _cycle + (BlockLink::SlotCycles - rest) : _cycle;
		}
	}

	uint64_t Hardware::blockLinkNow(const uint32_t _dspIndex)
	{
		auto& d = (_dspIndex == 0) ? m_dspMixer : m_dspProducer;
		return m_blockLink.base[_dspIndex] + (d.dsp().getCycles() - m_schedDspOriginCycles[_dspIndex]);
	}

	void Hardware::blockLinkInstall()
	{
		// On by default like the bounded dispatcher, with the serial path kept as the fallback and as the
		// reference for A/B comparisons.
		const auto* const env = std::getenv("GEARMULATOR_MDMM_BLOCK_LINK");
		m_blockLink.enabled = (env == nullptr || std::strcmp(env, "0") != 0)
			&& !isMonomachine() && m_firmwareFingerprint == g_mdOs163Fingerprint;
		if(!m_blockLink.enabled)
			return;

		using dsp56k::TWord;
		m_dspMixer.getPeriph().setDmaRegisterHook([this](const TWord _addr, TWord& _value, const bool _write)
		{
			return blockLinkMixerDma(_addr, _value, _write);
		});
		m_dspProducer.getPeriph().setDmaRegisterHook([this](const TWord _addr, TWord& _value, const bool _write)
		{
			return blockLinkProducerDma(_addr, _value, _write);
		});
	}

	void Hardware::blockLinkEngage()
	{
		auto& link = m_blockLink;
		if(link.active || !link.enabled)
			return;

		// The firmware fingerprint covers the image. Check the loaded programs as well, at the instructions
		// this depends on: the mixer's read of DDR4, the producer's wait on DCR0 and its read of the pin.
		const auto& mixerMemory = m_dspMixer.dsp().memory();
		const auto& producerMemory = m_dspProducer.dsp().memory();
		const bool programsMatch =
			mixerMemory.get(dsp56k::MemArea_P, 0x73) == 0x57f000
			&& mixerMemory.get(dsp56k::MemArea_P, 0x74) == 0xffffde
			&& producerMemory.get(dsp56k::MemArea_P, 0xcf) == 0x0aacb7
			&& producerMemory.get(dsp56k::MemArea_P, 0xbb) == 0x57f000;
		if(!programsMatch || !m_schedDspOriginLatched[0] || !m_schedDspOriginLatched[1])
		{
			link.enabled = false;
			return;
		}

		link.active = true;
		// This runs inside a peripheral access and can be inside a slot of either serial clock, so the slot
		// schedule itself is taken away at the next scheduler step.
		link.clockOffPending = true;
		link.quantumFrames = BlockLink::QuantumMicroseconds * static_cast<double>(g_samplerate) / 1.0e6;
		link.sendCount = 0;
		link.heldFirst = 0;
		link.windowArmed = false;

		for(uint32_t i = 0; i < 2; ++i)
		{
			auto& d = (i == 0) ? m_dspMixer : m_dspProducer;
			auto& periph = d.getPeriph();
			auto& essi = periph.getEssi0();

			link.base[i] = static_cast<uint64_t>(std::llround(
				m_schedDspOriginFrame[i] * static_cast<double>(g_cyclesPerFrame)));

			// Words keep arriving on the slot grid the port was running on
			uint64_t lastSlot = 0;
			uint32_t period = 0;
			link.gridPhase[i] = 0;
			if(periph.getEssiClock().getEsaiFineSchedule(&essi, lastSlot, period) && period == BlockLink::SlotCycles)
				link.gridPhase[i] = static_cast<uint32_t>(
					(link.base[i] + (lastSlot - m_schedDspOriginCycles[i])) % BlockLink::SlotCycles);

			essi.setClockGate([] { return false; });
			// What is still queued for this port was sent on the serial path and is not going to be received.
			// The port of DSP i receives the direction 1 - i of the transport scorecard.
			auto& ring = essi.getAudioInputs();
			recordMdLinkPurge(ring.size(), 1u - i);
			while(!ring.empty())
				ring.pop_front();

			// A link channel that the serial path left armed must not fire again
			auto& dma = periph.getDMA();
			for(const uint32_t channel : {0u, 2u, 4u})
			{
				const auto dcr = dma.getDCR(channel);
				const auto source = (dcr >> dsp56k::DmaChannel::Drs0) & 0x1f;
				if((dcr & g_dmaEnable) && (source == g_requestEssi0Receive || source == g_requestEssi0Transmit))
					dma.setDCR(channel, dcr & ~g_dmaEnable);
			}

			/*	Without the link's slots the only regular peripheral deadline left is the codec word, every 1152
				cycles. As an instruction count it is an estimate that runs late whenever the cycles per
				instruction change, which they do in every block when a DSP goes from its render into a wait loop:
				two codec words then ran back to back, and in some runs the output slipped by one sample for good.
			*/
			periph.getEssiClock().setExactBaseTickDeadline(true);
		}

		link.levelBefore = link.level = m_mdProducerPortCVisible;
		link.toggleTime = 0;
		link.pinLastRead = ~0u;
		link.producerWaiting = false;

		m_dspProducer.getPeriph().getPortC().setHostInputSource([this]() -> dsp56k::TWord
		{
			auto& l = m_blockLink;
			const auto now = blockLinkNow(1);
			// The pin is a wire: what it shows at the producer's time is only known once the mixer has got there
			if(blockLinkNow(0) < now && !m_schedInLinkDelivery)
				schedCatchUpDspToDsp(0, 1);
			const auto level = l.toggleTime <= now ? l.level : l.levelBefore;
			// The producer only reads the pin in its wait for the block toggle. Two equal reads in a row mean it
			// is in that wait, which the scheduler uses to let it follow the mixer instead of running ahead.
			l.producerWaiting = level == l.pinLastRead;
			l.pinLastRead = level;
			return level;
		});
	}

	void Hardware::blockLinkMixerPin(const dsp56k::TWord _level)
	{
		auto& link = m_blockLink;
		if(_level == link.level)
			return;
		link.levelBefore = link.level;
		link.level = _level;
		link.toggleTime = blockLinkNow(0);
		// The toggle starts the block: the producer's next send is track 1 of a new 512 word window, which the
		// mixer arms a few instructions from here.
		link.sendCount = 0;
		link.heldFirst = 0;
		link.windowArmed = false;
	}

	void Hardware::blockLinkDeliverHeld()
	{
		auto& link = m_blockLink;
		auto& mixerMemory = m_dspMixer.dsp().memory();
		for(; link.heldFirst < link.sendCount; ++link.heldFirst)
		{
			const auto destination = link.windowBase + link.heldFirst * BlockLink::TrackWords;
			for(uint32_t k = 0; k < BlockLink::TrackWords; ++k)
				mixerMemory.set(dsp56k::MemArea_Y, (destination + k) & 0xffffff, link.held[link.heldFirst][k]);
		}
	}

	bool Hardware::blockLinkMixerDma(const dsp56k::TWord _addr, dsp56k::TWord& _value, const bool _write)
	{
		auto& link = m_blockLink;
		if(!link.enabled)
			return false;

		auto& dma = m_dspMixer.getPeriph().getDMA();

		if(_write && _addr == dsp56k::XIO_DCR2 && (_value & g_dcrMask) == g_mixerSend)
		{
			// The mixer's block setup is the first thing either program does with the link, engage here
			blockLinkEngage();
			if(!link.active)
				return false;

			if(_value & g_dmaEnable)
			{
				const auto source = dma.getDSR(2);
				const auto& memory = m_dspMixer.dsp().memory();
				for(uint32_t k = 0; k < BlockLink::ReverseWords; ++k)
					link.reverse[k] = memory.get(dsp56k::MemArea_X, (source + k) & 0xffffff);
				link.reverseValid = true;
				if(link.reverseArmed)
					blockLinkDeliverReverse();
			}
			dma.setDCR(2, _value & ~g_dmaEnable);
			return true;
		}

		if(!link.active)
			return false;

		if(_write && _addr == dsp56k::XIO_DCR4 && (_value & g_dcrMask) == g_mixerReceive)
		{
			if(_value & g_dmaEnable)
			{
				link.windowBase = dma.getDDR(4);
				link.windowArmed = true;
				blockLinkDeliverHeld();
			}
			dma.setDCR(4, _value & ~g_dmaEnable);
			return true;
		}

		if(!_write && _addr == dsp56k::XIO_DDR4)
		{
			const auto now = blockLinkNow(0);
			/*	How many words have arrived by now is known if the block is complete, if a send is still under way
				(the producer does not arm the next one before it is done) or if the producer has reached this
				time. Otherwise the producer may yet arm a send whose first words arrive before now.
			*/
			const bool known = link.sendCount >= BlockLink::TrackCount
				|| (link.sendCount && now + BlockLink::SlotCycles < link.sendDone)
				|| blockLinkNow(1) >= now;
			if(!known && !m_schedInLinkDelivery)
				schedCatchUpDspToDsp(1, 0);

			uint32_t words = 0;
			for(uint32_t n = 0; n < link.sendCount; ++n)
			{
				if(now < link.firstArrival[n])
					break;
				words += static_cast<uint32_t>(std::min<uint64_t>(BlockLink::TrackWords,
					(now - link.firstArrival[n]) / BlockLink::SlotCycles + 1));
			}
			_value = ((link.windowArmed ? link.windowBase : dma.getDDR(4)) + words) & 0xffffff;
			return true;
		}

		return false;
	}

	void Hardware::blockLinkDeliverReverse()
	{
		auto& link = m_blockLink;
		auto& memory = m_dspProducer.dsp().memory();
		for(uint32_t k = 0; k < BlockLink::ReverseWords; ++k)
			memory.set(dsp56k::MemArea_X, (link.reverseDestination + k) & 0xffffff, link.reverse[k]);
		link.reverseValid = false;
		link.reverseArmed = false;
	}

	bool Hardware::blockLinkProducerDma(const dsp56k::TWord _addr, dsp56k::TWord& _value, const bool _write)
	{
		auto& link = m_blockLink;
		if(!link.active)
			return false;

		auto& dma = m_dspProducer.getPeriph().getDMA();

		if(_addr == dsp56k::XIO_DCR0)
		{
			if(!_write)
			{
				// DMA 0 clears its enable bit when the send is done, the producer waits for that
				_value = (dma.getDCR(0) & ~g_dmaEnable) | (blockLinkNow(1) < link.sendDone ? g_dmaEnable : 0);
				return true;
			}
			if(_value != (g_producerSend | g_dmaEnable))
				return false;

			const auto now = blockLinkNow(1);
			if(link.sendCount < BlockLink::TrackCount)
			{
				const auto source = dma.getDSR(0);
				const auto& memory = m_dspProducer.dsp().memory();
				auto& words = link.held[link.sendCount];
				for(uint32_t k = 0; k < BlockLink::TrackWords; ++k)
					words[k] = memory.get(dsp56k::MemArea_Y, (source + k) & 0xffffff);

				// One word per slot: it leaves on the producer's grid and arrives on the mixer's
				const uint64_t firstSent = slotAtOrAfter(now + BlockLink::TransmitOffset, link.gridPhase[1]);
				link.firstArrival[link.sendCount] = slotAtOrAfter(firstSent, link.gridPhase[0]);
				++link.sendCount;

				// The mixer does not look at a track before DDR4 says it is complete, so the words can be in
				// its buffer early. They only wait if the mixer has not armed its receive for this block yet.
				if(link.windowArmed)
					blockLinkDeliverHeld();
			}
			link.sendDone = now + BlockLink::TrackWords * BlockLink::SlotCycles;
			dma.setDCR(0, _value & ~g_dmaEnable);
			return true;
		}

		if(_write && _addr == dsp56k::XIO_DCR2 && (_value & g_dcrMask) == g_producerReceive)
		{
			if(_value & g_dmaEnable)
			{
				link.reverseDestination = dma.getDDR(2);
				link.reverseArmed = true;
				if(link.reverseValid)
					blockLinkDeliverReverse();
			}
			dma.setDCR(2, _value & ~g_dmaEnable);
			return true;
		}

		return false;
	}

	void Hardware::blockLinkSchedulerStep()
	{
		if(!m_blockLink.clockOffPending)
			return;
		m_blockLink.clockOffPending = false;
		for(auto* d : {&m_dspMixer, &m_dspProducer})
			d->getPeriph().getEssiClock().setEsaiFinePeriod(&d->getPeriph().getEssi0(), 0);
	}
}
