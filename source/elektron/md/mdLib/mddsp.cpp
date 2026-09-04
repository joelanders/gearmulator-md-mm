#include "mddsp.h"

#include "mdhardware.h"

#include "mc68k/hdi08.h"

namespace md
{
	using dsp56k::TWord;

	namespace
	{
		// DSP56303 memory sizing. XY is bridged into
		// P above g_bridgedAddr, so external SRAM (>= 0x020000) is shared between P and
		// XY. The Machinedrum second-stage loader uploads its main program into external
		// SRAM and jumps there, so sizeP() must span the external range.
		constexpr TWord g_pMemSize    = 0x800000;
		constexpr TWord g_xyMemSize   = 0x800000;
		constexpr TWord g_bridgedAddr = 0x020000;

		// Fill unused low P with a block-terminating op (RTS) so a stray jump there
		// compiles cleanly under the JIT.
		constexpr TWord g_trapFillEnd = 0x020000;
		constexpr TWord g_fillInstr   = 0x00000C;	// RTS

		// Select the model-specific DSP2 boot profile.
		constexpr uint8_t  g_hostCmd88Vector = 0x10;
		constexpr uint32_t g_bootQuery       = 0x147fff;
		constexpr uint32_t g_bootResponseMdUw = 0x65;
		constexpr uint32_t g_bootResponseMm   = 0x64;
	}

	Dsp::Dsp(Hardware& _hw, mc68k::Hdi08& _hdiUc, const uint32_t _index)
		: m_hardware(_hw)
		, m_hdiUC(_hdiUc)
		, m_index(_index)
		, m_buffer(dsp56k::Memory::calcMemSize(g_pMemSize, g_xyMemSize, g_bridgedAddr), 0)
		, m_memory(m_validator, g_pMemSize, g_xyMemSize, g_bridgedAddr, m_buffer.data())
		, m_dsp(m_memory, &m_periphX, &m_periphNop)
		, m_boot(m_dsp)
	{
		if(!_hw.isValid())
			return;

		// Enable the Monomachine-specific serial-output correction on DSP2 only.
		m_dsp.setMmCleanGndSin(m_hardware.isMonomachine() && m_index == 1);

		// Clock the serial ports from DSP cycles. At 101.6064 MHz, the 1152-cycle
		// codec slot and two slots per frame produce exactly 44.1 kHz; the firmware's
		// ESSI0 divider derives the 96-cycle inter-DSP link slot.
		m_periphX.getEssiClock().setExternalClockFrequency(10'240'000);
		m_periphX.getEssiClock().setSamplerate(44100);
		m_periphX.getEssiClock().setClockSource(dsp56k::EsxiClock::ClockSource::Cycles);
		m_periphX.getEssiClock().setExactCycleDeadlineEnabled(m_hardware.isMonomachine());

		// Fine-link mode must be active before the firmware writes CRA so ESSI0 can
		// run below the codec clock base. Synchronous receivers skip RX when their
		// wire is empty instead of fabricating a DMA word from the retained RX value.
		m_periphX.getEssiClock().setCyclesPerSample(1152u);
		m_periphX.getEssi0().setFineLinkMode(true);
		if(m_index == 0 || !m_hardware.isMonomachine())
		{
			m_periphX.getEssi0().setRxDataAvailableCallback([this]
			{
				return !m_periphX.getEssi0().getAudioInputs().empty();
			});

			// An RX0 read with DMA4 disabled flushes staged link data. DMA reads
			// occur with the channel enabled, which distinguishes the two cases.
			// This protocol is specific to Machinedrum DSP1.
			if(m_index == 0 && !m_hardware.isMonomachine())
			{
				m_periphX.getEssi0().setRxConsumeCallback([this]
				{
					if(m_periphX.getDMA().getDCR(4) & (1u << dsp56k::DmaChannel::De))
						return;
					auto& ring = m_periphX.getEssi0().getAudioInputs();
					while(!ring.empty())
						ring.pop_front();
					m_hardware.mdLinkWindowFlushed();
				});
			}
		}

		// Serialize host commands through the HI08 busy state so overlapping
		// commands cannot overwrite one another.
		hdi08().setHostCommandArbitration(true);

		auto config = m_dsp.getJit().getConfig();
		config.aguSupportBitreverse = true;
		// Keep eager child-block linking disabled because bootstrap targets may lie
		// outside the active program range.
		config.linkJitBlocks = false;
		config.dynamicPeripheralAddressing = false;
		// Loader execution can enter vector-area code as ordinary control flow, so
		// select the processing mode dynamically, as the other emulator hosts do.
		config.dynamicFastInterrupts = true;
		// Cap JIT block size so tight program loops return to the dispatcher often enough
		// for the peripherals (the ESSI cycle clock in particular) to be serviced; the
		// EssiClock ticks at most once per peripherals exec. MAME's hosting of this core
		// uses the same cap for the MD DSPs.
		config.maxInstructionsPerBlock = 32;
		// Likewise return from hardware DO loops regularly to service peripherals.
		config.maxDoIterations = 4;
		m_dsp.getJit().setConfig(config);
		m_dsp.getJit().preallocateBlockRuntimeData(
			RealtimeJitBlockRuntimeDataReserve);

		const TWord fillEnd = std::min<TWord>(g_trapFillEnd, m_memory.sizeP());
		for(TWord i = 0; i < fillEnd; ++i)
		{
			m_memory.set(dsp56k::MemArea_P, i, g_fillInstr);
			m_dsp.getJit().notifyProgramMemWrite(i);
		}

		// Keep the DSP from blocking on empty serial input during boot.
		m_periphX.getEssi0().writeEmptyAudioIn(64);
		m_periphX.getEssi1().writeEmptyAudioIn(64);

		hdi08().setRXRateLimit(0);
		hdi08().setTransmitDataAlwaysEmpty(false);

		// Monomachine uses a flow-controlled multi-word host stream. Buffer it here;
		// scheduler backpressure bounds the producer.
		if(m_hardware.isMonomachine())
			hdi08().setTransmitDataBuffered(true);

		// ---- Bridge the ColdFire-facing HI08 register file to the DSP (n2x model) ----

		m_hdiUC.setRxEmptyCallback([this](const bool _needMoreData)
		{
			onUCRxEmpty(_needMoreData);
		});

		// TX: during the boot upload each assembled 24-bit word drives DspBoot. Once boot
		// has finished the callback is switched to feed the running DSP's HORX.
		m_hdiUC.setWriteTxCallback([this](const uint32_t _word)
		{
			if(m_boot.hdiWriteTX(_word))
				onDspBootFinished();
		});

		m_hdiUC.setWriteIrqCallback([this](const uint8_t _irq)
		{
			hdiSendIrqToDSP(_irq);
		});

		m_hdiUC.setReadIsrCallback([this](const uint8_t _isr)
		{
			return hdiUcReadIsr(_isr);
		});

		// Derive TXDE/TRDY from the receive depth for this interface. Other products
		// retain the default always-ready behavior.
		m_hdiUC.setForceTxde(false);

		m_hdiUC.setInitHdi08Callback([this]
		{
			// Complete host-port initialization and report the transmitter ready.
			m_hdiUC.icr(m_hdiUC.icr() & 0x7f);
			m_hdiUC.isr(m_hdiUC.isr() | mc68k::Hdi08::IsrBits::Txde | mc68k::Hdi08::IsrBits::Trdy);
		});

	}

	void Dsp::onDspBootFinished()
	{
		// After boot, further host words are input to the DSP program.
		m_hdiUC.setWriteTxCallback([this](const uint32_t _word)
		{
			hdiTransferUCtoDSP(_word);
		});

		// There are no background DSP threads. Publish the completed boot state to
		// the deterministic scheduler that owns all subsequent execution.
		m_schedRunnable.store(true, std::memory_order_release);
	}

	namespace
	{
		// Cycle bound for a synchronous-HI08 inline DSP run (MAME's
		// time_catchup_max_cycles).
		uint64_t schedInlineClamp()
		{
			return 100'000;
		}
	}

	size_t Dsp::hostTxBacklog()
	{
		return hdi08().txData().size() + m_hdiUC.rxDataSize();
	}

	uint32_t Dsp::pumpHostRx(const size_t _maxUcWords)
	{
		// MAME (elektronmono.cpp) drains DSP2's HOTX into a host-side queue CONTINUOUSLY
		// (host_tx_queue) rather than demand-pulling one word at a time; that queue depth is what
		// raises HI08 HREQ (>= set_host_rx_irq_min_words words). Our UC-facing HI08 backing queue
		// (m_rxData) is that host-side queue: push DSP HOTX words straight into it, bypassing the
		// one-word RXDF latch gate in hdiTransferDSPtoUC (canReceiveData) which otherwise caps
		// availability at a single word and makes HREQ's >= 3 threshold unreachable. Bounded by
		// _maxUcWords so we don't grow it unbounded when the host isn't reading. This also fixes
		// the "HOTX is full, Discarding" overflow: DSP2's HOTX now drains promptly instead of only
		// when the firmware happens to demand a word.
		uint32_t moved = 0;
		while(m_hdiUC.rxDataSize() < _maxUcWords && hdi08().hasTX())
		{
			const auto w = hdi08().readTX();
			m_hdiUC.writeRx(w);
			++moved;
		}
		// If the host consumed the latched word on the previous instruction but more remain queued
		// (and no fresh DSP word re-latched them above), latch the next now so it is delivered in
		// FIFO order rather than read back as a spurious 0.
		m_hdiUC.relatchRx();
		return moved;
	}

	void Dsp::setHostPumpWakeCallback(const std::function<void()>& _callback)
	{
		hdi08().setHostPumpWakeCallback(_callback);
		m_hdiUC.setIcrWriteCallback([_callback](const uint8_t)
		{
			_callback();
		});
		m_hdiUC.setRxStateChangedCallback(_callback);
	}

	void Dsp::onUCRxEmpty(const bool _needMoreData)
	{
		m_hardware.notifyHostPumpStateChanged();

		if(_needMoreData)
		{
			// A blocking host read needs its peer to make progress on this single
			// scheduler thread, so run the target DSP inline until it produces the
			// reply or the in-flight host command has been fully serviced, bounded.
			const uint64_t clampStop = m_dsp.getCycles() + schedInlineClamp();
			while(!hdi08().hasTX()
				&& (hdi08().hostCommandBusy() || dsp().hasPendingInterrupts())
				&& m_dsp.getCycles() < clampStop)
				m_dsp.exec();
			hdiTransferDSPtoUC();
			return;
		}

		hdiTransferDSPtoUC();
	}

	void Dsp::hdiTransferUCtoDSP(const uint32_t _word)
	{
		// Catch the DSP up to the UC's current machine time before the word
		// lands (MAME catch_up_elapsed_time), so it consumes everything up to "now" first.
		m_hardware.schedCatchUpDsp(m_index);
		// Handle the MAME-compatible DSP2 boot response; other commands continue
		// through the emulated host interface.
		if(m_index == 1 && m_dsp2ReadyPeekArm)
		{
			m_dsp2ReadyPeekArm = false;

			if((_word & 0xffffff) == g_bootQuery)
			{
				m_hdiUC.writeRx(m_hardware.isMonomachine()
					? g_bootResponseMm : g_bootResponseMdUw);
				m_hardware.notifyHostPumpStateChanged();
				return;
			}

			// For an ordinary command, land the argument before dispatch.
			writeWordToDsp(_word);
			dispatchHostCommandInterrupt(g_hostCmd88Vector);
			return;
		}

		// Route ordinary data words through the paced host receive path. Host-command
		// arbitration keeps each argument with its in-flight command.
		writeWordToDsp(_word);
	}

	void Dsp::writeWordToDsp(const uint32_t _word)
	{
		// Preserve MM parameter-transfer ordering while the previous block is active.
		if(m_hardware.isMonomachine() && m_mmParamBlockVoice >= 0)
		{
			if(m_mmParamBlockWord == 0x28 && ((_word & 0xff) == 0x81 || (_word & 0xff) == 0x02))
			{
				const uint32_t targetHandle =
					0x528 + static_cast<uint32_t>(m_mmParamBlockVoice) * 0x100;
				if(m_dsp.memory().get(dsp56k::MemArea_Y, 0x123) == targetHandle)
				{
					const uint64_t clampStop = m_dsp.getCycles() + schedInlineClamp();
					while(m_dsp.memory().get(dsp56k::MemArea_Y, 0x123) == targetHandle
						&& m_dsp.getCycles() < clampStop)
						m_dsp.exec();
				}
			}
			if(++m_mmParamBlockWord >= 52)
				m_mmParamBlockVoice = -1;
		}

		// The DSP56303 HI08 host data path has a host latch and a one-word HRX. Before placing
		// a word in HRX, advance the target DSP until the previous word drains, bounded by the
		// scheduler clamp. This preserves MAME's feed_host_rx_queue invariant without a wall-clock
		// wait or an unbounded host-side FIFO.
		const uint64_t clampStop = m_dsp.getCycles() + schedInlineClamp();
		while(hdi08().hasRXData() && m_dsp.getCycles() < clampStop)
			m_dsp.exec();
		hdi08().writeRX(&_word, 1);
		return;
	}

	void Dsp::waitForHostCommandIdle()
	{
		// A CVR write may not overtake a host command already in flight. Hold the current
		// transaction in emulated time until RTI clears host-command-busy.
		if(!hdi08().hostCommandBusy())
			return;

		const uint64_t clampStop = m_dsp.getCycles() + schedInlineClamp();
		while(hdi08().hostCommandBusy() && m_dsp.getCycles() < clampStop)
			m_dsp.exec();
		return;
	}

	void Dsp::dispatchHostCommandInterrupt(const uint8_t _vba)
	{
		// Under the arbitration config, route the host-command vector through the DSP-side HDI08
		// so it raises HCP and arms the handler hold as it injects (native DSP56303 host-command
		// dispatch). Otherwise fall back to the legacy out-of-band inject (default uitest path).
		if(hdi08().hostCommandArbitration())
			hdi08().writeHostCommand(_vba);
		else
			dsp().injectExternalInterrupt(_vba);
	}

	void Dsp::hdiSendIrqToDSP(const uint8_t _irq)
	{
		// Catch the DSP up to the UC's current machine time before the CVR is
		// dispatched (MAME catch_up_elapsed_time), so HCP is raised at a defined point in DSP time.
		if(booted())
			m_hardware.schedCatchUpDsp(m_index);
		if(m_hardware.isMonomachine() && booted() && _irq >= 0x10 && _irq <= 0x14
			&& (_irq & 1) == 0)
		{
			m_mmParamBlockVoice = static_cast<int32_t>((_irq - 0x10) >> 1);
			m_mmParamBlockWord = 0;
		}
		// Preserve Monomachine host-command ordering. Data words precede the next
		// command, so drain the receive path before dispatching that command. Run the DSP
		// inline until HORX has drained before dispatching the CVR. This is needed
		// only for the MM transport.
		const bool s_mmInOrderCvr = m_hardware.isMonomachine();
		if(s_mmInOrderCvr && booted())
		{
			const uint64_t clampStop = m_dsp.getCycles() + schedInlineClamp() * 4;
			while(!hdi08().rxData().empty() && m_dsp.getCycles() < clampStop)
				m_dsp.exec();
		}

		if(!booted())
		{
			// Pre-boot: nothing to interrupt.
			return;
		}

		// Serialize host commands before dispatch. The HI08 command bit remains busy
		// until the current handler returns, keeping the following argument words with
		// the correct command.
		waitForHostCommandIdle();

		// Arm the MAME-compatible DSP2 boot acknowledgement. Other commands with
		// this vector are dispatched when their argument arrives.
		if(m_index == 1 && _irq == g_hostCmd88Vector)
		{
			m_dsp2ReadyPeekArm = true;
			return;
		}

		dispatchHostCommandInterrupt(_irq);

		hdiTransferDSPtoUC();
	}

	uint8_t Dsp::hdiUcReadIsr(uint8_t _isr)
	{
		// Catch the DSP up to the UC's current machine time before reporting
		// status, so a UC status-poll loop sees the DSP's progress (e.g. a reply it is waiting for)
		// in fine lockstep instead of a frozen snapshot. MAME runs a status slice at the same point.
		m_hardware.schedCatchUpDsp(m_index);
		hdiTransferDSPtoUC();

		// Mirror the DSP's host flags HF2/HF3 into the UC-visible ISR.
		const auto hf23 = hdi08().readControlRegister() & 0x18;	// HF2 (bit3), HF3 (bit4)
		_isr &= ~0x18;
		_isr |= static_cast<uint8_t>(hf23);

		// Model the two-stage HI08 transmit path described by DSP56303UM 6.3.6/6.6.8:
		// TXDE reports room in the host latch, while TRDY additionally requires the
		// DSP receive latch to be empty.
		const auto horxDepth = hdi08().rxData().size();
		_isr &= static_cast<uint8_t>(~(mc68k::Hdi08::IsrBits::Txde | mc68k::Hdi08::IsrBits::Trdy));
		if(horxDepth == 0)
			_isr |= mc68k::Hdi08::IsrBits::Txde | mc68k::Hdi08::IsrBits::Trdy;
		else if(horxDepth == 1)
			_isr |= mc68k::Hdi08::IsrBits::Txde;
		// HREQ is routed separately; composing it here would require the unmodelled IVR path.

		return _isr;
	}

	bool Dsp::hdiTransferDSPtoUC()
	{
		const bool hasTx = hdi08().hasTX();
		if(m_hdiUC.canReceiveData() && hasTx)
		{
			const auto echo = hdi08().readTX();
			m_hdiUC.writeRx(echo);
			m_hardware.notifyHostPumpStateChanged();
			return true;
		}
		return false;
	}
}
