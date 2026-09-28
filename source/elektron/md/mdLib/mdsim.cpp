#include "mdsim.h"

#include "mdplusdrive.h"

#include <algorithm>
#include <utility>

// md::Sim - MCF5206e SIM peripheral model. See mdsim.h for the register map and
// UM (MCF5206e User's Manual) citations. Implementation notes inline below.

namespace md
{

	Sim::Sim()
	{
		reset();
	}

	void Sim::reset()
	{
		m_mem.fill(0);
		m_timerPending = 0;

		// Documented power-on values for the few registers whose reset state the
		// firmware may observe before it programs them. Everything else resets to 0.
		m_mem[g_simr] = 0xc0;			// UM 8.3.2.2 SIMR reset $C0 (FRZ1:0=1, others 0)

		m_mem[g_imr    ] = 0x3f;		// UM 8.3.2.4 IMR reset $3FFE (all maskable ints masked),
		m_mem[g_imr + 1] = 0xfe;		//            stored big-endian

		m_ppInputLevel = 0xff;			// MD parallel-port inputs idle HIGH

		if(m_plusDrive)
			m_plusDrive->resetTransaction();

		for(auto& t : m_timer)
			t = Timer{};
		refreshTimerConfiguration(0, g_timer1Base);
		refreshTimerConfiguration(1, g_timer2Base);
		m_timerIrqInjected = {};
		m_uartTxIrqArmed = {};
		m_uartRxIrqArmed = {};
		m_interruptCheckNeeded = false;

		m_extIrq4Level = false;

		for(auto& u : m_uart)
		{
			u.rx.clear();
			u.rxOverflows = 0;
			u.mode = {};
			u.modePointer = 0;
			u.txEnabled = false;
			u.txHoldingFull = false;
			u.txHolding = 0;
			u.txShiftBusy = false;
			u.txShift = 0;
			u.txCyclesRemaining = 0;
			// u.txCallback is wiring, deliberately preserved across reset.
		}
	recomputeMatchBase();
	}

	// -------------------------------------------------------------------------
	// Byte access - all behavioural special cases live here; 16/32-bit accessors
	// decompose into big-endian byte accesses so they share this logic.
	// -------------------------------------------------------------------------

	uint8_t Sim::read8(const uint32_t _offset)
	{
		if(_offset >= g_windowSize)
			return 0;

		// Parallel port data (UM 10.3.2.2): input pins read their pin level (idle
		// HIGH on the MD), output pins read back the driven latch value. This is the
		// load-bearing "btst #0 of PPDAT" boot gate.
		if(_offset == g_ppdat)
		{
			const auto value = computeParallelData();
			traceParallelAccess(false, _offset, value);
			return value;
		}
		if(_offset == g_ppddr)
			traceParallelAccess(false, _offset, m_mem[g_ppddr]);

		// UART status register (UM 12.4.1.3): computed, never the stored UCSR that a
		// write to this same offset leaves behind.
		if(_offset == g_uart1Base + g_uartUsr)	return computeUartStatus(g_uartMidi);
		if(_offset == g_uart2Base + g_uartUsr)	return computeUartStatus(g_uartPanel);

		// UISR is a source-status read, not a readback of the write-only UIMR at
		// the same offset (MCF5206EUM 12.4.1.10/.11). Masking cannot hide status.
		if(_offset == g_uart1Base + g_uartIsr)	return computeUartInterruptStatus(g_uartMidi);
		if(_offset == g_uart2Base + g_uartIsr)	return computeUartInterruptStatus(g_uartPanel);

		// UART receiver buffer (UM 12.4.1.4 URB): pops the RX FIFO.
		if(_offset == g_uart1Base + g_uartRxTx)	return popReceiveBuffer(g_uartMidi);
		if(_offset == g_uart2Base + g_uartRxTx)	return popReceiveBuffer(g_uartPanel);

		// Timer counters (UM 14.4.1.4 TCN): a read yields the live counter, not a stored
		// value. TCN is 16-bit big-endian at base+$0C. Timer event flags (TER) likewise
		// reflect matches up to now: fold pending timer time first.
		if(_offset == g_timer1Base + g_timerTcn)		{ flushTimers(); return static_cast<uint8_t>(m_timer[0].counter >> 8); }
		if(_offset == g_timer1Base + g_timerTcn + 1)	{ flushTimers(); return static_cast<uint8_t>(m_timer[0].counter); }
		if(_offset == g_timer2Base + g_timerTcn)		{ flushTimers(); return static_cast<uint8_t>(m_timer[1].counter >> 8); }
		if(_offset == g_timer2Base + g_timerTcn + 1)	{ flushTimers(); return static_cast<uint8_t>(m_timer[1].counter); }
		if(_offset == g_timer1Base + g_timerTer
			|| _offset == g_timer2Base + g_timerTer)	{ flushTimers(); }

		return m_mem[_offset];
	}

	uint16_t Sim::read16(const uint32_t _offset)
	{
		return static_cast<uint16_t>((static_cast<uint16_t>(read8(_offset)) << 8) | read8(_offset + 1));
	}

	uint32_t Sim::read32(const uint32_t _offset)
	{
		return (static_cast<uint32_t>(read16(_offset)) << 16) | read16(_offset + 2);
	}

	namespace
	{
		// Timer register bytes whose write interacts with timer time (mode /
		// reference / counter / event state): pending time must be folded in
		// first so the action anchors at the current counter. Capture
		// registers (TCR) are deliberately excluded: the model never
		// evaluates them, so they are pure stores.
		bool isTimerStateOffset(const uint32_t _offset)
		{
			switch(_offset)
			{
			case Sim::g_timer1Base + Sim::g_timerTmr:
			case Sim::g_timer1Base + Sim::g_timerTmr + 1:
			case Sim::g_timer1Base + Sim::g_timerTrr:
			case Sim::g_timer1Base + Sim::g_timerTrr + 1:
			case Sim::g_timer1Base + Sim::g_timerTcn:
			case Sim::g_timer1Base + Sim::g_timerTcn + 1:
			case Sim::g_timer1Base + Sim::g_timerTer:
			case Sim::g_timer2Base + Sim::g_timerTmr:
			case Sim::g_timer2Base + Sim::g_timerTmr + 1:
			case Sim::g_timer2Base + Sim::g_timerTrr:
			case Sim::g_timer2Base + Sim::g_timerTrr + 1:
			case Sim::g_timer2Base + Sim::g_timerTcn:
			case Sim::g_timer2Base + Sim::g_timerTcn + 1:
			case Sim::g_timer2Base + Sim::g_timerTer:
				return true;
			default:
				return false;
			}
		}

		bool isTimerCaptureOffset(const uint32_t _offset)
		{
			return _offset == Sim::g_timer1Base + Sim::g_timerTcr
				|| _offset == Sim::g_timer1Base + Sim::g_timerTcr + 1
				|| _offset == Sim::g_timer2Base + Sim::g_timerTcr
				|| _offset == Sim::g_timer2Base + Sim::g_timerTcr + 1;
		}
	}

	void Sim::write8(const uint32_t _offset, const uint8_t _value)
	{
		if(_offset >= g_windowSize)
			return;

		// Anchor timer-interacting writes at current timer time: a pending
		// span belongs to the old register configuration (e.g. a TCN reset or
		// a prescaler change must apply to up-to-date counters). Pure stores
		// (including the timer capture registers, which nothing evaluates)
		// need no fold-in: the pending span stays pending.
		if(isTimerStateOffset(_offset))
			flushTimers();

		// A register write can unmask or configure a source. Be conservative here;
		// takeNextInterrupt() clears the gate after one full scan when the write was
		// unrelated to interrupt state. Timer capture registers are exempt: no
		// evaluation path reads them, so they can neither unmask nor configure.
		if(!isTimerCaptureOffset(_offset))
			m_interruptCheckNeeded = true;

		// Interrupt Pending Register is read-only (UM 8.3.2.5); writes have no effect.
		if(_offset == g_ipr || _offset == g_ipr + 1)
			return;

		// Timer counter (UM 14.4.1.4): "a write of any value to TCN causes it to reset to
		// all zeros." TCN is not part of the stored register window.
		if(_offset == g_timer1Base + g_timerTcn || _offset == g_timer1Base + g_timerTcn + 1)
		{
			m_timer[0] = Timer{};
			refreshTimerConfiguration(0, g_timer1Base);
			recomputeMatchBase();
			return;
		}
		if(_offset == g_timer2Base + g_timerTcn || _offset == g_timer2Base + g_timerTcn + 1)
		{
			m_timer[1] = Timer{};
			refreshTimerConfiguration(1, g_timer2Base);
			recomputeMatchBase();
			return;
		}

		// Timer Event Register (UM 14.4.1.5): write-1-to-clear. Writing a one clears the
		// corresponding event bit (and, once REF/CAP are both clear, negates the IRQ);
		// writing a zero leaves it unchanged.
		if(_offset == g_timer1Base + g_timerTer || _offset == g_timer2Base + g_timerTer)
		{
			m_mem[_offset] &= static_cast<uint8_t>(~(_value & (g_terRef | g_terCap)));
			// Clearing REF negates the timer IRQ line - re-arm so the next match injects.
			if(_value & g_terRef)
				m_timerIrqInjected[_offset == g_timer2Base + g_timerTer ? 1 : 0] = false;
			return;
		}

		// UIMR enabling an already-asserted TxRDY source makes it serviceable.
		if(_offset == g_uart1Base + g_uartIsr && (_value & g_uimrTxRdy)
			&& (computeUartStatus(g_uartMidi) & g_usrTxRdy))
			m_uartTxIrqArmed[g_uartMidi] = true;
		if(_offset == g_uart2Base + g_uartIsr && (_value & g_uimrTxRdy)
			&& (computeUartStatus(g_uartPanel) & g_usrTxRdy))
			m_uartTxIrqArmed[g_uartPanel] = true;

		// RX readiness is retained by queue/pop independently of UIMR (UM 12.4.1.11).
		// Mask writes only gate delivery; rearming here would duplicate an offer
		// already handed to the CPU, including across a disable/enable sequence.

		if(_offset == g_ppddr || _offset == g_ppdat)
			traceParallelAccess(true, _offset, _value);

		// Default behaviour: every register (PPDDR/PPDAT latch, chip selects, timers,
		// UART mode/clock/command config, interrupt controller, ...) is stored so a
		// subsequent read returns what was written.
		m_mem[_offset] = _value;

		// +Drive transmit snoop: the device decodes send_raw commands from the
		// PPDDR=$0c write stream so receive windows can answer statefully.
		if(m_plusDrive)
		{
			if(_offset == g_ppddr)
				m_plusDrive->onPPDDRWrite(_value);
			else if(_offset == g_ppdat)
				m_plusDrive->onPPDATWrite(_value, m_mem[g_ppddr]);
		}

		if(_offset == g_uart2Base + g_uartMr)
			writeUartMode(g_uartPanel, _value);
		else if(_offset == g_uart2Base + g_uartCr)
			writeUartCommand(g_uartPanel, _value);

		const auto refreshIfTimerConfig = [this, _offset](const unsigned _index,
			const uint32_t _base)
		{
			const bool tmr = _offset == _base + g_timerTmr
				|| _offset == _base + g_timerTmr + 1;
			const bool trr = _offset == _base + g_timerTrr
				|| _offset == _base + g_timerTrr + 1;
			if(tmr || trr)
				refreshTimerConfiguration(_index, _base);
		};
		refreshIfTimerConfig(0, g_timer1Base);
		refreshIfTimerConfig(1, g_timer2Base);

	// A timer configuration change re-times the next reference match.
	if(isTimerStateOffset(_offset))
		recomputeMatchBase();

		// UART transmit buffer (UM 12.4.1.5 UTB): capture the byte for later routing.
		// The transmitter is always modelled ready, so writes are accepted unthrottled.
		if(_offset == g_uart1Base + g_uartRxTx)		pushTransmitBuffer(g_uartMidi, _value);
		else if(_offset == g_uart2Base + g_uartRxTx)	pushTransmitBuffer(g_uartPanel, _value);
	}

	void Sim::write16(const uint32_t _offset, const uint16_t _value)
	{
		write8(_offset,     static_cast<uint8_t>(_value >> 8));
		write8(_offset + 1, static_cast<uint8_t>(_value & 0xff));
	}

	void Sim::write32(const uint32_t _offset, const uint32_t _value)
	{
		write16(_offset,     static_cast<uint16_t>(_value >> 16));
		write16(_offset + 2, static_cast<uint16_t>(_value & 0xffff));
	}

	// -------------------------------------------------------------------------
	// Parallel port
	// -------------------------------------------------------------------------

	// Port A tracer. PPDDR ($1c5) and PPDAT ($1c9) byte accesses only. The +Drive
	// hangs off Port A per the MAME elektronmono.cpp driver notes (bit 2 address,
	// bit 3 unknown, bits 4-7 data RX/TX, detected by communicating with it), so
	// this log is the evidence for a future +Drive responder. One line per access,
	// stamped with the parent CPU's cycle counter; writes show the raw byte while
	// reads additionally show DDR/output-latch/input level for context.
	void Sim::traceParallelAccess(const bool _isWrite, const uint32_t _offset,
		const uint8_t _value)
	{
		if(!m_portATraceEnabled || !m_portATraceClock)
			return;
		const auto cycles = *m_portATraceClock;
		const auto pc = m_portATracePc ? *m_portATracePc : 0;
		if(_isWrite)
			std::fprintf(stderr, "[PORTA] %012llu W %s $%02x pc=%06x\n",
				static_cast<unsigned long long>(cycles),
				_offset == g_ppdat ? "PPDAT" : "PPDDR", _value, pc);
		else
			std::fprintf(stderr,
				"[PORTA] %012llu R %s -> $%02x (ddr=$%02x out=$%02x in=$%02x) pc=%06x\n",
				static_cast<unsigned long long>(cycles),
				_offset == g_ppdat ? "PPDAT" : "PPDDR", _value,
				m_mem[g_ppddr], m_mem[g_ppdat], m_ppInputLevel, pc);
	}

	uint8_t Sim::computeParallelData() const
	{
		// UM 10.3.2.1/10.3.2.2: DDR bit 1 = output, 0 = input.
		//   output bits -> last value written to PPDAT (the pin drive latch)
		//   input  bits -> the level present on the pin (idle-high on the MD)
		const uint8_t ddr    = m_mem[g_ppddr];
		const uint8_t outLat = m_mem[g_ppdat];
		uint8_t inputLevel = m_ppInputLevel;
		if(m_mk2PortAInvertedLoopback)
		{
			// MKII board identification uses an inverted Port A loopback: input
			// bit 0 reads the inverse of driven output bit 2.
			inputLevel = static_cast<uint8_t>(
				(inputLevel & ~uint8_t{0x01}) | ((~outLat & 0x04) >> 2));
		}
		// +Drive responder (receive phase only: PPDDR=$04 drives the clock but
		// releases data bits). The device decides the data line level; command
		// decoding and the menu-visible behaviour live in mdplusdrive.cpp.
		// NOTE: when attached the device *replaces* the idle level on bits
		// 3-7 (bit3 = presence/data, bits 4-7 = +Drive data bus per MAME
		// elektronmono.cpp and the 0x23eedc upper-nibble poll), it does not OR
		// with it — otherwise a drive-low would be masked by idle-high.
		// Bits 0-2 (MKII strap, battery, clock) always observe the board.
		uint8_t inputMasked = inputLevel;
		if(m_plusDrive && (ddr & 0x04) && !(ddr & 0x08))
		{
			const uint32_t pc = m_portATracePc ? *m_portATracePc : 0;
			const uint8_t drive = m_plusDrive->dataBitForRead(outLat, pc);
			inputMasked = static_cast<uint8_t>(
				(inputLevel & ~uint8_t{0xF8}) | (drive & 0xF8));
		}
		return static_cast<uint8_t>(
			(ddr & outLat) | (static_cast<uint8_t>(~ddr) & inputMasked));
	}

	// -------------------------------------------------------------------------
	// UART
	// -------------------------------------------------------------------------

	uint8_t Sim::computeUartStatus(const unsigned _uart) const
	{
		// UM 12.4.1.3 Status Register. UART1 retains immediate legacy delivery.
		// Once UART2 uses its internal baud generator, TxRDY describes the holding
		// register and TxEMP additionally requires an idle shift register.
		uint8_t usr = 0;

		if(_uart < g_uartCount)
		{
			const auto& uart = m_uart[_uart];
			if(_uart != g_uartPanel || !panelTransmitTimingActive())
				usr |= g_usrTxEmp | g_usrTxRdy;
			else if(uart.txEnabled)
			{
				if(!uart.txHoldingFull)
					usr |= g_usrTxRdy;
				if(!uart.txHoldingFull && !uart.txShiftBusy)
					usr |= g_usrTxEmp;
			}
			const auto& rx = uart.rx;
			if(!rx.empty())
				usr |= g_usrRxRdy;
			if(rx.size() >= 3)		// FIFO depth is 3 (UM 12.4.1.3 FFULL)
				usr |= g_usrFFull;
		}
		return usr;
	}

	uint8_t Sim::computeUartInterruptStatus(const unsigned _uart) const
	{
		// Report the ready sources implemented by this UART model. RXIRQ's
		// FIFO-full selection, delta-break, and CTS-change status remain unmodelled;
		// this separates status/mask aliases without claiming a complete UART.
		const auto usr = computeUartStatus(_uart);
		return ((usr & g_usrTxRdy) ? g_uimrTxRdy : 0)
			| ((usr & g_usrRxRdy) ? g_uimrRxRdy : 0);
	}

	uint8_t Sim::popReceiveBuffer(const unsigned _uart)
	{
		if(_uart >= g_uartCount)
			return 0;

		auto& rx = m_uart[_uart].rx;
		if(rx.empty())
			return 0;

		uint8_t b = 0;
		rx.pop(b);
		++m_uart[_uart].rxConsumed;

		// RxRDY stays asserted while the FIFO is nonempty (UM 12.4.1.3/.6).
		// In this edge-offer model, consuming URB permits an offer for the next
		// byte, even if currently masked. Draining the FIFO clears the source.
		m_uartRxIrqArmed[_uart] = !rx.empty();
		if(m_uartRxIrqArmed[_uart])
			m_interruptCheckNeeded = true;
		return b;
	}

	void Sim::pushTransmitBuffer(const unsigned _uart, const uint8_t _value)
	{
		if(_uart >= g_uartCount)
			return;

		auto& u = m_uart[_uart];
		if(_uart == g_uartPanel && panelTransmitTimingActive())
		{
			// UTB ignores writes while disabled or while its one-byte holding register
			// is occupied (UM 12.4.1.7).
			if(!u.txEnabled || u.txHoldingFull)
				return;
			u.txHolding = _value;
			u.txHoldingFull = true;
			startPanelShiftRegister();
			return;
		}

		if(u.txCallback)
			u.txCallback(_value);

		// The ISR just sent a byte; if its transmit interrupt is still enabled, re-arm it so
		// the next TxRDY fires and the ring keeps draining until the ISR disables UIMR.
		const uint32_t base = (_uart == g_uartPanel) ? g_uart2Base : g_uart1Base;
		if(m_mem[base + g_uartIsr] & g_uimrTxRdy)
		{
			m_uartTxIrqArmed[_uart] = true;
			m_interruptCheckNeeded = true;
		}
	}

	void Sim::writeUartMode(const unsigned _uart, const uint8_t _value)
	{
		if(_uart >= g_uartCount)
			return;
		auto& uart = m_uart[_uart];
		uart.mode[uart.modePointer] = _value;
		if(uart.modePointer == 0)
			uart.modePointer = 1;
	}

	void Sim::writeUartCommand(const unsigned _uart, const uint8_t _value)
	{
		if(_uart >= g_uartCount)
			return;
		auto& uart = m_uart[_uart];
		switch((_value >> 4) & 7)
		{
			case 1:
				uart.modePointer = 0;
				break;
			case 3:
				uart.txEnabled = false;
				uart.txHoldingFull = false;
				uart.txShiftBusy = false;
				uart.txCyclesRemaining = 0;
				m_uartTxIrqArmed[_uart] = false;
				break;
			default:
				break;
		}

		switch((_value >> 2) & 3)
		{
			case 1:
				uart.txEnabled = true;
				if(computeUartStatus(_uart) & g_usrTxRdy)
					armTransmitReady(_uart);
				break;
			case 2:
				uart.txEnabled = false;
				uart.txHoldingFull = false;
				m_uartTxIrqArmed[_uart] = false;
				break;
			default:
				break;
		}
	}

	bool Sim::panelTransmitTimingActive() const
	{
		const auto divisor = static_cast<uint16_t>(
			(static_cast<uint16_t>(m_mem[g_uart2Base + g_uartBg1]) << 8)
			| m_mem[g_uart2Base + g_uartBg2]);
		return (m_mem[g_uart2Base + g_uartUsr] & 0x0f) == 0x0d && divisor >= 2;
	}

	uint32_t Sim::panelCharacterCycles() const
	{
		const auto& uart = m_uart[g_uartPanel];
		const uint32_t divisor =
			(static_cast<uint32_t>(m_mem[g_uart2Base + g_uartBg1]) << 8)
			| m_mem[g_uart2Base + g_uartBg2];
		const uint32_t dataBits = 5 + (uart.mode[0] & 3);
		const uint32_t parityBits = ((uart.mode[0] >> 3) & 3) == 2 ? 0 : 1;
		const uint32_t stopSixteenths = 9 + (uart.mode[1] & 0x0f);
		const uint32_t frameSixteenths = 16 + dataBits * 16 + parityBits * 16
			+ stopSixteenths;
		return frameSixteenths * 2 * divisor;
	}

	void Sim::armTransmitReady(const unsigned _uart)
	{
		const auto base = _uart == g_uartPanel ? g_uart2Base : g_uart1Base;
		if((m_mem[base + g_uartIsr] & g_uimrTxRdy)
			&& (computeUartStatus(_uart) & g_usrTxRdy))
		{
			m_uartTxIrqArmed[_uart] = true;
			m_interruptCheckNeeded = true;
		}
	}

	void Sim::startPanelShiftRegister()
	{
		auto& uart = m_uart[g_uartPanel];
		if(!uart.txEnabled || uart.txShiftBusy || !uart.txHoldingFull)
			return;
		uart.txShift = uart.txHolding;
		uart.txHoldingFull = false;
		uart.txShiftBusy = true;
		uart.txCyclesRemaining = panelCharacterCycles();
		armTransmitReady(g_uartPanel);
	}

	void Sim::stepPanelTransmitter(uint32_t _cycles)
	{
		auto& uart = m_uart[g_uartPanel];
		while(uart.txShiftBusy && _cycles >= uart.txCyclesRemaining)
		{
			_cycles -= uart.txCyclesRemaining;
			uart.txCyclesRemaining = 0;
			uart.txShiftBusy = false;
			if(uart.txCallback)
				uart.txCallback(uart.txShift);
			startPanelShiftRegister();
		}
		if(uart.txShiftBusy)
			uart.txCyclesRemaining -= _cycles;
	}

	void Sim::setTransmitCallback(const unsigned _uart, TransmitCallback _callback)
	{
		if(_uart >= g_uartCount)
			return;
		m_uart[_uart].txCallback = std::move(_callback);
	}

	bool Sim::tryQueueRx(const unsigned _uart, const uint8_t _byte)
	{
		if(_uart >= g_uartCount)
			return false;
		auto& uart = m_uart[_uart];
		if(!uart.rx.push(_byte))
		{
			++uart.rxOverflows;
			return false;
		}

		// A newly nonempty FIFO creates a receive source regardless of UIMR.
		// Appending behind an unread byte must not duplicate its existing offer;
		// popReceiveBuffer rearms when the next byte becomes the FIFO head.
		if(uart.rx.size() == 1)
		{
			m_uartRxIrqArmed[_uart] = true;
			m_interruptCheckNeeded = true;
		}
		return true;
	}

	void Sim::queueRx(const unsigned _uart, const uint8_t _byte)
	{
		(void)tryQueueRx(_uart, _byte);
	}

	bool Sim::hasQueuedRx(const unsigned _uart) const
	{
		return _uart < g_uartCount && !m_uart[_uart].rx.empty();
	}

	size_t Sim::queuedRxBytes(const unsigned _uart) const
	{
		return _uart < g_uartCount ? m_uart[_uart].rx.size() : 0;
	}

	size_t Sim::availableRxBytes(const unsigned _uart) const
	{
		return _uart < g_uartCount ? m_uart[_uart].rx.available() : 0;
	}

	size_t Sim::rxOverflowCount(const unsigned _uart) const
	{
		return _uart < g_uartCount ? m_uart[_uart].rxOverflows : 0;
	}

	uint64_t Sim::rxConsumedCount(const unsigned _uart) const
	{
		return _uart < g_uartCount ? m_uart[_uart].rxConsumed : 0;
	}

	// -------------------------------------------------------------------------
	// Timers + interrupt controller (UM Section 14 / 8.3.2)
	// -------------------------------------------------------------------------

	namespace
	{
		// Number of integers v in the half-open range (_a, _b] with v congruent to _r
		// modulo _m. Used to detect whether a free-running counter (which advances by a
		// whole instruction's worth of cycles at a time) passed its reference value.
		uint32_t numCongruent(int64_t _a, int64_t _b, int64_t _m, int64_t _r)
		{
			auto floorDiv = [](int64_t _x, int64_t _y) { return _x >= 0 ? _x / _y : -(( -_x + _y - 1) / _y); };
			return static_cast<uint32_t>(floorDiv(_b - _r, _m) - floorDiv(_a - _r, _m));
		}
	}

	uint16_t Sim::reg16(const uint32_t _offset) const
	{
		return static_cast<uint16_t>((static_cast<uint16_t>(m_mem[_offset]) << 8) | m_mem[_offset + 1]);
	}

	void Sim::refreshTimerConfiguration(const unsigned _index, const uint32_t _base)
	{
		const uint16_t tmr = reg16(_base + g_timerTmr);
		auto& timer = m_timer[_index];
		const uint8_t iclk = static_cast<uint8_t>((tmr & g_tmrIclk) >> 1);
		timer.running = (tmr & g_tmrRst) && iclk != 0 && iclk != 3;
		const uint32_t clockDiv = (iclk == 2) ? 16 : 1;
		const uint32_t prescale = ((tmr >> g_tmrPsShift) & 0xff) + 1;
		timer.div = clockDiv * prescale;
		timer.reference = reg16(_base + g_timerTrr);
		timer.period = static_cast<uint32_t>(timer.reference) + 1;
		timer.freeRunning = (tmr & g_tmrFrr) == 0;
	}

	void Sim::exec(const uint32_t _cycles)
	{
		if(!m_lazyTimers)
		{
			// Historical per-call stepping, preserved exactly (order included)
			// as the field fallback.
			stepTimer(0, g_timer1Base, _cycles);
			stepTimer(1, g_timer2Base, _cycles);
			stepPanelTransmitter(_cycles);
			return;
		}
		// Panel UART completion callbacks stay synchronous (contract tested in
		// uartRegisterTest); only the timers go lazy (see the header).
		stepPanelTransmitter(_cycles);
		m_timerPending += _cycles;
		// Fold exactly at a reference match so TER latch + gate land on the
		// same cycle eager stepping would have used (bit-exact); the cap is a
		// backstop for match-free spans.
		if(m_timerPending >= m_matchBase || m_timerPending >= kTimerFlushCap)
			flushTimers();
	}

	void Sim::flushTimers()
	{
		if(!m_timerPending)
			return;
		// Pending spans are short (flushed on every timer observation and
		// capped above), so they always fit the 32-bit steppers.
		const auto pending = static_cast<uint32_t>(m_timerPending);
		m_timerPending = 0;
		stepTimer(0, g_timer1Base, pending);
		stepTimer(1, g_timer2Base, pending);
		recomputeMatchBase();
	}

	// Cycles from the given counter/frac state until the next reference match,
	// with exactly the arithmetic the deadline query below has always used.
	static uint64_t referenceMatchIn(const uint32_t _counter, const uint32_t _frac,
		const uint32_t _reference, const uint32_t _period,
		const bool _freeRunning, const uint32_t _div)
	{
		uint32_t ticksUntilMatch;
		if(!_freeRunning)
			ticksUntilMatch = _counter >= _period ? 1 : _period - _counter;
		else
		{
			ticksUntilMatch = (_reference - _counter) & 0xffff;
			if(!ticksUntilMatch)
				ticksUntilMatch = 0x10000;
		}
		return uint64_t{ticksUntilMatch} * _div - _frac;
	}

	void Sim::recomputeMatchBase()
	{
		uint64_t best = UINT64_MAX;
		for(unsigned index = 0; index < 2; ++index)
		{
			const auto& timer = m_timer[index];
			if(!timer.running)
				continue;
			const uint64_t match = referenceMatchIn(timer.counter, timer.frac,
				timer.reference, timer.period, timer.freeRunning, timer.div);
			best = std::min(best, match);
		}
		m_matchBase = best;
	}

	uint32_t Sim::cyclesUntilNextUartTransmit() const
	{
		const auto& uart = m_uart[g_uartPanel];
		return uart.txShiftBusy ? uart.txCyclesRemaining : g_noTimerInterruptDeadline;
	}

	uint32_t Sim::cyclesUntilNextTimerInterrupt() const
	{
		const uint32_t timer1 = cyclesUntilTimerInterrupt(
			0, g_timer1Base, g_irqSrcTimer1);
		const uint32_t timer2 = cyclesUntilTimerInterrupt(
			1, g_timer2Base, g_irqSrcTimer2);
		return timer1 < timer2 ? timer1 : timer2;
	}

	uint32_t Sim::cyclesUntilTimerInterrupt(const unsigned _index,
		const uint32_t _base, const uint32_t _sourceBit) const
	{
		const uint16_t tmr = reg16(_base + g_timerTmr);
		if(!(tmr & g_tmrRst) || !(tmr & g_tmrOri)
			|| imrMasked(_sourceBit) || m_timerIrqInjected[_index])
			return g_noTimerInterruptDeadline;

		if(m_mem[_base + g_timerTer] & g_terRef)
			return 0;

		const uint8_t iclk = static_cast<uint8_t>((tmr & g_tmrIclk) >> 1);
		if(iclk == 0 || iclk == 3)
			return g_noTimerInterruptDeadline;

		const auto& timer = m_timer[_index];
		// The counters below are at the last flush; the match lies this many
		// cycles after that. A match inside the still-pending span is already
		// due now (its TER latches on flush); otherwise report what remains.
		// With an empty pending span this is exactly the historical value.
		const uint64_t matchBase = referenceMatchIn(timer.counter, timer.frac,
			timer.reference, timer.period, timer.freeRunning, timer.div);
		if(matchBase <= m_timerPending)
			return 0;
		const uint64_t remaining = matchBase - m_timerPending;
		return remaining >= g_noTimerInterruptDeadline
			? g_noTimerInterruptDeadline : static_cast<uint32_t>(remaining);
	}

	void Sim::stepTimer(const unsigned _index, const uint32_t _base, const uint32_t _cycles)
	{
		auto& timer = m_timer[_index];
		if(!timer.running)
			return;

		timer.frac += _cycles;
		if(timer.frac < timer.div)
			return;
		const uint32_t ticks = timer.frac / timer.div;
		timer.frac -= ticks * timer.div;

		const uint32_t before = timer.counter;
		const uint32_t total = before + ticks;
		bool matched;
		if(!timer.freeRunning)
		{
			matched = total >= timer.period;
			timer.counter = static_cast<uint16_t>(matched ? total % timer.period : total);
		}
		else
		{
			matched = numCongruent(before, total, 0x10000,
				timer.reference) != 0;
			timer.counter = static_cast<uint16_t>(total & 0xffff);
		}
		if(matched)
		{
			m_mem[_base + g_timerTer] |= g_terRef;
			m_interruptCheckNeeded = true;
		}
	}

	bool Sim::timerAssertsIrq(const uint32_t _base) const
	{
		// The timer drives its IRQ line when the REF event is latched AND the output-
		// reference interrupt is enabled (UM 14.4.1.1 ORI note). Capture events are not
		// used by the MD, so the CAP path is not modelled.
		const uint16_t tmr = reg16(_base + g_timerTmr);
		return (m_mem[_base + g_timerTer] & g_terRef) && (tmr & g_tmrOri);
	}

	bool Sim::imrMasked(const uint32_t _sourceBit) const
	{
		return (reg16(g_imr) >> _sourceBit) & 1;
	}

	uint8_t Sim::icrLevel(const uint32_t _icrOffset) const
	{
		return static_cast<uint8_t>((m_mem[_icrOffset] >> 2) & 7);	// IL2-IL0 (UM 8.3.2.3)
	}

	void Sim::setExternalIrq4(const bool _level)
	{
		// UM 8.3.2.3: external IRQ4 is level-sensitive. Just store the pin level; the CPU-facing
		// re-offer + de-dup is handled by md::Microcontroller's pending-state bookkeeping.
		m_extIrq4Level = _level;
	}

	bool Sim::getExternalIrq4(uint8_t& _level, uint8_t& _vector)
	{
		// External IRQ4 is the DSP2 HI08 HREQ line. It is level-sensitive and
		// uses the configured ICR (MCF5206E UM 8.3.2.3).
		if(!m_extIrq4Level || imrMasked(g_irqSrcExtIrq4))
			return false;

		_level  = icrLevel(g_icrExtIrq4);
		_vector = static_cast<uint8_t>(24 + _level);	// external IRQ pins autovector (vector 24+IL)
		return true;
	}

	bool Sim::takeNextInterrupt(uint8_t& _level, uint8_t& _vector)
	{
		// Evaluate against current timer state: a reference match inside the
		// pending span must be latched (TER + gate) before this scan.
		flushTimers();
		if(!m_interruptCheckNeeded)
			return false;

		// Highest-priority sources first. The UARTs (level 3) outrank the timers (level 1).

		// UART1 (MIDI) / UART2 (panel) transmitter-ready. Programmed vector via UIVR when the
		// ICR AVEC bit is clear (the MD programs it clear), else the ColdFire autovector.
		struct UartIrq { unsigned uart; uint32_t base, icr, src; };
		static constexpr UartIrq uarts[] =
		{
			{ g_uartPanel, g_uart2Base, g_icrUart2, g_irqSrcUart2 },
			{ g_uartMidi,  g_uart1Base, g_icrUart1, g_irqSrcUart1 },
		};
		for(const auto& u : uarts)
		{
			if(imrMasked(u.src))
				continue;

			const uint8_t uimr = m_mem[u.base + g_uartIsr];
			bool fire = false;

			// Offer RX once for the current FIFO head, retaining readiness across masks.
			// A URB read rearms for remaining data; mask writes do not create new offers.
			// Check RX first so input is serviced promptly.
			if(m_uartRxIrqArmed[u.uart] && (uimr & g_uimrRxRdy) && !m_uart[u.uart].rx.empty())
			{
				m_uartRxIrqArmed[u.uart] = false;
				fire = true;
			}
			// Transmitter-ready: UIMR TxRDY enabled since armed. Re-armed on each UTB write.
			else if(m_uartTxIrqArmed[u.uart])
			{
				m_uartTxIrqArmed[u.uart] = false;
				if(!(uimr & g_uimrTxRdy))	// TX interrupt disabled since armed - drop it
					continue;
				fire = true;
			}

			if(!fire)
				continue;

			_level  = icrLevel(u.icr);
			_vector = (m_mem[u.icr] & g_icrAutovector) ? static_cast<uint8_t>(24 + _level)
													  : m_mem[u.base + g_uartIvr];
			return true;
		}

		// Timers 1/2 (level, autovectored). Inject once per REF match; the latch re-arms when
		// the ISR clears TER (see write8).
		const struct { uint32_t base, icr, src; unsigned idx; } timers[] =
		{
			{ g_timer1Base, g_icrTimer1, g_irqSrcTimer1, 0 },
			{ g_timer2Base, g_icrTimer2, g_irqSrcTimer2, 1 },
		};
		for(const auto& t : timers)
		{
			if(m_timerIrqInjected[t.idx] || !timerAssertsIrq(t.base) || imrMasked(t.src))
				continue;
			m_timerIrqInjected[t.idx] = true;
			_level  = icrLevel(t.icr);
			_vector = static_cast<uint8_t>(24 + _level);	// timers always autovector
			return true;
		}

		m_interruptCheckNeeded = false;
		return false;
	}
}
