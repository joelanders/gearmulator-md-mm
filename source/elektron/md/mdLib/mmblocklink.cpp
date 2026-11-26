#include "mdhardware.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mdtransportpolicy.h"

#include "dsp56kEmu/dma.h"

// The block link of the Monomachine, see mmblocklink.h for what it replaces and why.

namespace md
{
	namespace
	{
		constexpr dsp56k::TWord g_dmaEnable = 1u << dsp56k::DmaChannel::De;

		// ESSI0 as a DMA request source (DSP56303 UM, DRS field)
		constexpr dsp56k::TWord g_requestEssi0Receive = 0b01010;
		constexpr dsp56k::TWord g_requestEssi0Transmit = 0b01011;

		constexpr dsp56k::TWord g_producerSendBlock = 0xca5a20;		// DMA 1: 4 lines of X memory -> TX00, interrupt
		constexpr uint64_t g_cyclesPerFrame = 2304;					// DSP cycles per codec frame, both DSPs

		// The first slot of a 96-cycle grid at or after a cycle
		uint64_t slotAtOrAfter(const uint64_t _cycle, const uint64_t _anchor)
		{
			const auto period = static_cast<int64_t>(MmBlockLink::SlotCycles);
			auto rest = (static_cast<int64_t>(_cycle) - static_cast<int64_t>(_anchor)) % period;
			if(rest < 0)
				rest += period;
			return rest ? _cycle + static_cast<uint64_t>(period - rest) : _cycle;
		}

		bool isDcr(const dsp56k::TWord _addr, uint32_t& _channel)
		{
			switch(_addr)
			{
			case dsp56k::XIO_DCR0: _channel = 0; return true;
			case dsp56k::XIO_DCR1: _channel = 1; return true;
			case dsp56k::XIO_DCR2: _channel = 2; return true;
			case dsp56k::XIO_DCR3: _channel = 3; return true;
			case dsp56k::XIO_DCR4: _channel = 4; return true;
			case dsp56k::XIO_DCR5: _channel = 5; return true;
			default: return false;
			}
		}
	}


	void Hardware::mmBlockLinkInstall()
	{
		// Off unless asked for: not bit-identical to the serial path, measured in render tools only.
		const auto* const env = std::getenv("GEARMULATOR_MM_BLOCK_LINK");
		m_mmBlockLink.enabled = env != nullptr && std::strcmp(env, "1") == 0
			&& isMonomachine() && m_firmwareFingerprint == g_mmOs132bFingerprint;
		if(!m_mmBlockLink.enabled)
			return;

		using dsp56k::TWord;
		// DSP2's DMA registers: the arms of its link channel are the block link's, the poll of the codec DMA
		// pointer stays with the wait loop skip (mmSpinSkipInstall, which has run before this)
		m_dspProducer.getPeriph().setDmaRegisterHook([this](const TWord _addr, TWord& _value, const bool _write)
		{
			if(mmBlockLinkProducerDma(_addr, _value, _write))
				return true;
			return m_mmSpinSkip && mmSpinSkipProducerDma(_addr, _value, _write);
		});
		m_dspProducer.getPeriph().getEssiClock().setCycleAlarmCallback([this] { mmBlockLinkSendDone(); });
		m_dspMixer.getPeriph().getEssiClock().setCycleAlarmCallback([this] { mmBlockLinkDeliver(); });
	}

	void Hardware::mmBlockLinkReport() const
	{
		const auto& link = m_mmBlockLink;
		if(!link.enabled && !link.active)
			return;
		const auto* const env = std::getenv("GEARMULATOR_MM_BLOCK_LINK_STATS");
		if(env == nullptr || std::strcmp(env, "1") != 0)
			return;
		std::fprintf(stderr, "MMBLOCKLINK active=%d bursts=%llu words=%llu late=%llu maxLateCycles=%llu "
			"receiverNotArmed=%llu shortReceives=%llu dropped=%llu unsupported=%llu pinEdges=%llu pinMaxDelay=%llu\n", link.active ? 1 : 0,
			static_cast<unsigned long long>(link.bursts), static_cast<unsigned long long>(link.words),
			static_cast<unsigned long long>(link.lateDeliveries), static_cast<unsigned long long>(link.maxLateCycles),
			static_cast<unsigned long long>(link.receiverNotArmed), static_cast<unsigned long long>(link.shortReceives),
			static_cast<unsigned long long>(link.droppedBursts), static_cast<unsigned long long>(link.unsupportedPrograms),
			static_cast<unsigned long long>(link.pinEdges), static_cast<unsigned long long>(link.pinMaxDelay));
	}

	void Hardware::mmBlockLinkEngage()
	{
		auto& link = m_mmBlockLink;
		if(link.active || !link.enabled)
			return;

		// Both DSPs have to be past their boot and on the shared clock, and their serial ports on the slot grid
		if(!m_schedDspOriginLatched[0] || !m_schedDspOriginLatched[1])
			return;

		// The firmware fingerprint covers the image. Check the loaded programs as well, at the instructions
		// this depends on: the two arms of each DSP's link channel and DSP2's read of the pin.
		const auto& mixerMemory = m_dspMixer.dsp().memory();
		const auto& producerMemory = m_dspProducer.dsp().memory();
		const auto p = [](const dsp56k::Memory& _memory, const dsp56k::TWord _addr)
		{
			return _memory.get(dsp56k::MemArea_P, _addr);
		};
		const bool programsMatch =
			p(mixerMemory, 0x1a0) == 0x08f49c && p(mixerMemory, 0x1a1) == 0xca5140		// DMA 4: RX0 -> 4 lines
			&& p(mixerMemory, 0x25f) == 0x08f49c && p(mixerMemory, 0x260) == 0x8a52c0	// DMA 4: RX0 -> 48 words
			&& p(mixerMemory, 0x20) == 0x0bb4a4											// DMA 4 interrupt
			&& p(producerMemory, 0x195) == 0x044fdd										// the pin
			&& p(producerMemory, 0x1a7) == 0x08f4a8 && p(producerMemory, 0x1a8) == g_producerSendBlock
			&& p(producerMemory, 0x217) == 0x08f4a8 && p(producerMemory, 0x218) == 0x8a5a50
			&& p(producerMemory, 0x1a) == 0x0bb4a1;										// DMA 1 interrupt

		uint32_t period[2] = {0, 0};
		uint64_t lastSlot[2] = {0, 0};
		bool onGrid = true;
		for(uint32_t i = 0; i < 2; ++i)
		{
			auto& periph = ((i == 0) ? m_dspMixer : m_dspProducer).getPeriph();
			onGrid = onGrid && periph.getEssiClock().getEsaiFineSchedule(&periph.getEssi0(), lastSlot[i], period[i])
				&& period[i] == MmBlockLink::SlotCycles;
		}
		if(!programsMatch || !onGrid)
		{
			link.enabled = false;
			return;
		}

		link.active = true;
		link.gridAnchor = {lastSlot[0], lastSlot[1]};
		link.sendPending = false;
		link.lastSendSlot = 0;
		link.arrivingFirst = link.arrivingCount = 0;

		// The pin, the scheduler's follow rule and the removal of the slot schedule are the Machinedrum's
		auto& shared = m_blockLink;
		shared.active = true;
		shared.clockOffPending = true;
		shared.quantumFrames = transportPolicy(m_model).backgroundQuantumMicroseconds
			* static_cast<double>(g_samplerate) / 1.0e6;

		for(uint32_t i = 0; i < 2; ++i)
		{
			auto& d = (i == 0) ? m_dspMixer : m_dspProducer;
			auto& periph = d.getPeriph();
			auto& essi = periph.getEssi0();

			shared.base[i] = static_cast<uint64_t>(std::llround(
				m_schedDspOriginFrame[i] * static_cast<double>(g_cyclesPerFrame)));

			essi.setClockGate([] { return false; });
			// What is still queued was sent on the serial path, before the request this engages on
			auto& ring = essi.getAudioInputs();
			while(!ring.empty())
				ring.pop_front();

			// Without the link's slots the codec word is the only regular peripheral deadline, see mdblocklink.cpp
			periph.getEssiClock().setExactBaseTickDeadline(true);
		}
		m_mmLinkAwaitFresh.store(false, std::memory_order_release);

		// The pin as DSP2 sees it now. With the timed block sync an edge that DSP1 has driven at a time DSP2 has
		// not reached yet is still on its way and keeps its time.
		shared.toggleTime = 0;
		if(m_mmTimedSync)
		{
			const auto producerNow = m_dspProducer.dsp().getCycles();
			auto level = m_mmSyncLevel;
			size_t n = 0;
			while(n < m_mmSyncEdges.size() && m_mmSyncEdges[n].first <= producerNow)
				level = m_mmSyncEdges[n++].second;
			shared.levelBefore = shared.level = level;
			if(n < m_mmSyncEdges.size())
			{
				const auto& edge = m_mmSyncEdges.back();
				shared.level = edge.second;
				shared.toggleTime = shared.base[1] + (edge.first - m_schedDspOriginCycles[1]);
			}
			m_mmSyncEdges.clear();
			m_mmSyncLevel = level;
		}
		else
		{
			shared.levelBefore = shared.level = m_dspMixer.getPeriph().getPortC().hostRead() & (1u << 1);
		}
		shared.pinLastRead = ~0u;
		shared.producerWaiting = false;

		m_dspProducer.getPeriph().getPortC().setHostInputSource([this]() -> dsp56k::TWord
		{
			auto& l = m_blockLink;
			const auto now = blockLinkNow(1);
			// The pin is a wire: what it shows at DSP2's time is only known once DSP1 has got there
			if(blockLinkNow(0) < now && !m_schedInLinkDelivery)
				schedCatchUpDspToDsp(0, 1);
			const auto level = l.toggleTime <= now ? l.level : l.levelBefore;
			if(m_mmSpinSkip)
			{
				// The level stays as read until the next toggle DSP1 has made, and is not known beyond DSP1's own
				// time, at which it may yet toggle
				uint64_t until = blockLinkNow(0);
				if(l.toggleTime > now)
					until = std::min(until, l.toggleTime);
				auto& dsp = m_dspProducer.dsp();
				const bool loop = dsp.getPC().toWord() == 0x195
					&& dsp.memory().get(dsp56k::MemArea_P, 0x195) == 0x044fdd
					&& dsp.memory().get(dsp56k::MemArea_P, 0x198) == 0x05a7dd;
				spinSkip(m_spinMmPin, dsp, loop ? 0x195 : ~0u, level, until > now ? dsp.getCycles() + (until - now) : 0);
			}
			if(level != l.pinLastRead && l.pinLastRead != ~0u && l.toggleTime <= now)
			{
				++m_mmBlockLink.pinEdges;
				m_mmBlockLink.pinMaxDelay = std::max(m_mmBlockLink.pinMaxDelay, now - l.toggleTime);
			}
			// DSP2 only reads the pin in its wait for the block toggle. Two equal reads in a row mean it is in
			// that wait, which the scheduler uses to let it follow DSP1 instead of running ahead.
			l.producerWaiting = level == l.pinLastRead;
			l.pinLastRead = level;
			return level;
		});
	}

	bool Hardware::mmBlockLinkProducerDma(const dsp56k::TWord _addr, dsp56k::TWord& _value, const bool _write)
	{
		auto& link = m_mmBlockLink;
		uint32_t channel = 0;
		if(!link.enabled || !_write || !isDcr(_addr, channel) || !(_value & g_dmaEnable))
			return false;
		if(((_value >> dsp56k::DmaChannel::Drs0) & 0x1f) != g_requestEssi0Transmit)
			return false;

		// DSP2's send of a block is the first thing either program does with the link in a block, engage here
		if(!link.active)
		{
			if(_value != g_producerSendBlock)
				return false;
			mmBlockLinkEngage();
			if(!link.active)
				return false;
		}

		auto& dsp = m_dspProducer.dsp();
		auto& dma = m_dspProducer.getPeriph().getDMA();

		// The words the channel is going to send, in the order its address generation walks them. Their buffers
		// belong to the block DSP2 has finished and are not written again before the send is over.
		const auto sourceMode = (_value >> dsp56k::DmaChannel::Dam0) & 7;
		const auto destinationMode = (_value >> dsp56k::DmaChannel::Dam3) & 7;
		const bool threeD = (_value >> dsp56k::DmaChannel::D3d) & 1;
		const auto transferMode = (_value >> dsp56k::DmaChannel::Dtm0) & 7;
		const auto area = dsp56k::DmaChannel::convertMemArea(_value & 3);
		const auto count = dma.getDCO(channel);
		uint32_t lines = 1, lineWords = 0;
		dsp56k::TWord lineStep = 1;
		if(sourceMode <= 3)
		{
			lines = (count >> 12) + 1;
			lineWords = (count & 0xfff) + 1;
			lineStep = dma.getDOR(sourceMode);
		}
		else if(sourceMode == 5)
			lineWords = count + 1;
		const auto wordCount = lines * lineWords;
		if(threeD || !wordCount || wordCount > MmBlockLink::MaxBurstWords || destinationMode != 4 || transferMode != 1
			|| area == dsp56k::MemArea_COUNT || link.sendPending)
		{
			++link.unsupportedPrograms;
			return false;
		}

		// A DSP1 that has not taken the oldest burst by now has lost it, as a receiver that is not served would
		if(link.arrivingCount == MmBlockLink::MaxBursts)
		{
			link.arrivingFirst = (link.arrivingFirst + 1) % MmBlockLink::MaxBursts;
			--link.arrivingCount;
			++link.droppedBursts;
		}
		auto& burst = link.arriving[(link.arrivingFirst + link.arrivingCount) % MmBlockLink::MaxBursts];
		burst.count = 0;
		const auto& memory = dsp.memory();
		auto address = dma.getDSR(channel);
		for(uint32_t l = 0; l < lines; ++l)
		{
			for(uint32_t w = 0; w < lineWords; ++w)
			{
				burst.words[burst.count++] = memory.get(area, address);
				address = (address + (w + 1 < lineWords ? 1 : lineStep)) & 0xffffff;
			}
		}

		// The channel is armed as on the serial path. ESSI0 is not clocked, so no request reaches it until this
		// runs it to its end.
		dma.setDCR(channel, _value);

		/*	When the channel fetches its last word and when that word leaves, see mmblocklink.h. Traced on the
			serial path: a 128 word send armed at cycle a with the register empty fetched its last word on slot
			S + 126 and sent it on S + 127, S being the first slot at or after a; the 48 word send armed from its
			interrupt, 21 cycles later, fetched its last word on S + 174 and sent it on S + 175.
		*/
		const auto now = dsp.getCycles();
		const auto slot = slotAtOrAfter(now, link.gridAnchor[1]);
		const bool registerFull = link.lastSendSlot >= now;
		uint64_t doneSlot, firstSendSlot, sendSlot;
		if(registerFull)
		{
			const auto first = std::max(slot, link.lastSendSlot);
			firstSendSlot = first + MmBlockLink::SlotCycles;
			doneSlot = first + static_cast<uint64_t>(wordCount - 1) * MmBlockLink::SlotCycles;
			sendSlot = doneSlot + MmBlockLink::SlotCycles;
		}
		else
		{
			firstSendSlot = slot;
			sendSlot = slot + static_cast<uint64_t>(wordCount - 1) * MmBlockLink::SlotCycles;
			doneSlot = wordCount > 1 ? sendSlot - MmBlockLink::SlotCycles : now;
		}

		link.sendPending = true;
		link.sendChannel = channel;
		link.sendWords = wordCount;
		link.lastSendSlot = sendSlot;
		m_dspProducer.getPeriph().getEssiClock().setCycleAlarm(doneSlot);

		// ... and a word arrives on the first slot of DSP1 that is not earlier than the one it leaves on
		const auto arrival = [&](const uint64_t _producerSlot)
		{
			const auto machine = m_blockLink.base[1] + (_producerSlot - m_schedDspOriginCycles[1]);
			const auto mixer = m_schedDspOriginCycles[0] + (machine > m_blockLink.base[0] ? machine - m_blockLink.base[0] : 0);
			return slotAtOrAfter(mixer, link.gridAnchor[0]);
		};
		burst.storeCycle = arrival(firstSendSlot);
		burst.deliverCycle = arrival(sendSlot);
		burst.stored = false;

		const auto mixerNow = m_dspMixer.dsp().getCycles();
		if(mixerNow > burst.deliverCycle)
		{
			++link.lateDeliveries;
			link.maxLateCycles = std::max(link.maxLateCycles, mixerNow - burst.deliverCycle);
		}
		++link.bursts;
		link.words += wordCount;

		if(link.arrivingCount++ == 0)
			m_dspMixer.getPeriph().getEssiClock().setCycleAlarm(burst.storeCycle);
		return true;
	}

	void Hardware::mmBlockLinkSendDone()
	{
		// DSP2 has reached the slot on which the channel would have fetched its last word: run the channel to
		// its end. It walks its addresses and counters as it would have, clears its enable bit and raises its
		// interrupt. What it writes to the transmit register goes nowhere, the words were taken when the
		// channel was armed.
		auto& link = m_mmBlockLink;
		if(!link.sendPending)
			return;
		link.sendPending = false;

		m_dspProducer.getPeriph().getDMA().execRequests(link.sendChannel, link.sendWords);
	}

	void Hardware::mmBlockLinkDeliver()
	{
		auto& link = m_mmBlockLink;
		if(!link.arrivingCount)
			return;

		auto& periph = m_dspMixer.getPeriph();
		auto& dma = periph.getDMA();
		auto& clock = periph.getEssiClock();

		auto& burst = link.arriving[link.arrivingFirst];
		const auto now = m_dspMixer.dsp().getCycles();
		if(now < burst.storeCycle)
		{
			clock.setCycleAlarm(burst.storeCycle);
			return;
		}

		const auto dcr4 = dma.getDCR(4);
		const bool armed = (dcr4 & g_dmaEnable) && ((dcr4 >> dsp56k::DmaChannel::Drs0) & 0x1f) == g_requestEssi0Receive;

		// From the arrival of its first word the burst is in DSP1's buffer, where DMA 4 is going to put it. The
		// channel of the second send of a block is armed by the interrupt of the first, a slot before
		if(!burst.stored && armed)
		{
			dma.previewRequests(4, burst.count, burst.words.data());
			burst.stored = true;
		}
		if(now < burst.deliverCycle)
		{
			clock.setCycleAlarm(burst.stored ? burst.deliverCycle : std::min(burst.deliverCycle, now + MmBlockLink::SlotCycles));
			return;
		}

		// The last word has arrived: DMA 4 ends its transfer as if the receiver had presented the words one per
		// slot, and raises its interrupt. Words that find the channel not armed are lost, as they would be to a
		// receiver overrun.
		uint32_t moved = 0;
		if(armed)
			moved = dma.execRequests(4, burst.count, burst.words.data());
		else
			++link.receiverNotArmed;
		if(moved && moved < burst.count)
			++link.shortReceives;

		link.arrivingFirst = (link.arrivingFirst + 1) % MmBlockLink::MaxBursts;
		--link.arrivingCount;

		// The next burst is not looked at before DSP1 had a slot to serve the interrupt of this one
		if(link.arrivingCount)
			clock.setCycleAlarm(std::max(link.arriving[link.arrivingFirst].storeCycle, now + MmBlockLink::SlotCycles));
	}
}
