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

	/*	Exact skip of a wait loop that polls one register.

		Called at a read of the register with the value the read returns and the first DSP cycle at which a read
		could return something else (~0 if only a peripheral pass can change it). The block that reads must begin
		at the loop's known address (the PC a block runs under is its first address). Two earlier reads with the same
		value, the same PC and the same distance in cycles and instructions as this one mean the DSP is spinning
		on it: the loop is a fixed sequence that only depends on the value read. The next k iterations are then
		accounted in one step if nothing could have happened at any of the block boundaries they contain:

		- no interrupt is queued (the dispatcher would take it at the next boundary),
		- no peripheral becomes due, by cycle or by instruction count (peripherals run only at boundaries, and
		  only they change the DMA registers; their deadlines do not move unless they run),
		- the run does not reach its stop cycle (only runs through execUntilCycles are skipped),
		- every skipped read would have returned the same value.

		Inside a block that accesses a peripheral the counters are added after the block, so at the read they
		hold the cycle at which the block began. One iteration of margin is kept so that this holds even if the
		loop spans two blocks. The read then happens k iterations later, with the same result, so registers,
		memory, counters and the boundaries at which anything happens are the same as without the skip.
	*/
	void Hardware::spinSkip(SpinSkip& _s, dsp56k::DSP& _dsp, const dsp56k::TWord _loopPc, const dsp56k::TWord _value, const uint64_t _validUntilCycle)
	{
		++_s.reads;
		uint64_t cycles = _dsp.getCycles();
		uint64_t instructions = _dsp.getInstructionCounter();
		const dsp56k::TWord pc = _dsp.getPC().toWord();
		const uint64_t dc = cycles - _s.cycles;
		const uint64_t di = instructions - _s.instructions;

		const bool spinning = _s.have && pc == _loopPc && _value == _s.value && pc == _s.pc && dc && di
			&& dc == _s.deltaCycles && di == _s.deltaInstructions;

		if(spinning && _dsp.isInterruptFuncPlain())
		{
			const uint64_t stop = _dsp.getRunStopCycle();
			if(stop)
			{
				const auto* periph = _dsp.getPeriph(0);
				const uint64_t limitCycle = std::min({stop, *periph->getTargetCyclePtr(), _validUntilCycle});
				const uint64_t limitInstruction = periph->getTargetClock();
				// k iterations such that the boundaries up to one iteration beyond them stay below the limits
				uint64_t k = limitCycle > cycles ? (limitCycle - cycles) / dc : 0;
				k = std::min(k, limitInstruction > instructions ? (limitInstruction - instructions) / di : 0);
				if(k > 1)
				{
					--k;
					_dsp.fastForward(static_cast<dsp56k::TWord>(k * di), static_cast<dsp56k::TWord>(k * dc));
					cycles += k * dc;
					instructions += k * di;
					++_s.skips;
					_s.skippedIterations += k;
					_s.skippedCycles += k * dc;
				}
			}
		}

		_s.deltaCycles = dc;
		_s.deltaInstructions = di;
		_s.cycles = cycles;
		_s.instructions = instructions;
		_s.value = _value;
		_s.pc = pc;
		_s.have = true;
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

		{
			// Exact skip of the wait loops (spinSkip). GEARMULATOR_MD_SPIN_SKIP=0 switches it off
			const auto* const spin = std::getenv("GEARMULATOR_MD_SPIN_SKIP");
			m_spinSkipEnabled = spin == nullptr || std::strcmp(spin, "0") != 0;
			m_spinSkipStats = std::getenv("GEARMULATOR_MD_SPIN_STATS") != nullptr;
		}

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
		// The wait loops spinSkip knows: the producer's on the pin ($BB) and on DCR0 ($CF, both checked above),
		// the mixer's on the codec DMA pointer ($3C, a read of DDR0)
		if(mixerMemory.get(dsp56k::MemArea_P, 0x3c) != 0x084e2e)
			m_spinSkipEnabled = false;
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
			if(m_spinSkipEnabled)
			{
				// The level stays as read until the next toggle the mixer has made, and is not known beyond
				// the mixer's own time, at which it may yet toggle
				uint64_t until = blockLinkNow(0);
				if(l.toggleTime > now)
					until = std::min(until, l.toggleTime);
				auto& dsp = m_dspProducer.dsp();
				spinSkip(m_spinPin, dsp, 0xbb, level, until > now ? dsp.getCycles() + (until - now) : 0);
			}
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

		if(!_write && _addr == dsp56k::XIO_DDR0 && m_spinSkipEnabled)
		{
			// The codec DMA pointer, which the mixer polls for its block phase. Only a DMA transfer moves it,
			// and transfers only happen in a peripheral pass.
			_value = dma.getDDR(0);
			spinSkip(m_spinDdr0, m_dspMixer.dsp(), 0x3c, _value, ~uint64_t(0));
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
				const auto now = blockLinkNow(1);
				_value = (dma.getDCR(0) & ~g_dmaEnable) | (now < link.sendDone ? g_dmaEnable : 0);
				if(m_spinSkipEnabled)
				{
					// the enable bit clears when the send is done and does not change before
					auto& dsp = m_dspProducer.dsp();
					spinSkip(m_spinDcr0, dsp, 0xcf, _value, now < link.sendDone ? dsp.getCycles() + (link.sendDone - now) : ~uint64_t(0));
				}
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
