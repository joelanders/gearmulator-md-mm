#include "mdsim.h"

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

		// Documented power-on values for the few registers whose reset state the
		// firmware may observe before it programs them. Everything else resets to 0.
		m_mem[g_simr] = 0xc0;			// UM 8.3.2.2 SIMR reset $C0 (FRZ1:0=1, others 0)

		m_mem[g_imr    ] = 0x3f;		// UM 8.3.2.4 IMR reset $3FFE (all maskable ints masked),
		m_mem[g_imr + 1] = 0xfe;		//            stored big-endian

		m_ppInputLevel = 0xff;			// MD parallel-port inputs idle HIGH

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
			// u.txCallback is wiring, deliberately preserved across reset.
		}
	}

	void Sim::prepareFirmwareUpdateBoot(const bool _monomachine)
	{
		// Chip-select and board glue setup observed at the documented main-OS
		// handoff. The differing values describe the MD and MM physical maps; all
		// UART/port values below are ordinary MCF5206E programming-model fields.
		const auto set = [this](const uint32_t _offset, const uint8_t _value)
		{
			write8(_offset, _value);
		};
		if(!_monomachine)
		{
			set(0x047, 0x25); set(0x04a, 0x40); set(0x04d, 0x20);
			set(0x051, 0x0e); set(0x057, 0x27);
		}
		set(0x069, 0x0f);
		set(0x06e, _monomachine ? 0x15 : 0x19);
		set(0x06f, _monomachine ? 0xc3 : 0xdf);
		set(0x071, 0x10); set(0x075, 0x0f);
		set(0x07a, _monomachine ? 0x0d : 0x19);
		set(0x07b, _monomachine ? 0xdb : 0xdf);
		set(0x07d, 0x50); set(0x086, 0x01); set(0x087, 0x47);
		set(0x089, 0x60); set(0x092, 0x01); set(0x093, 0x47);
		if(_monomachine)
		{
			set(0x0a1, 0x20); set(0x0a5, 0x0f);
			set(0x0aa, 0x01); set(0x0ab, 0x83);
		}
		set(0x0cb, 0x82);

		set(g_uart1Base + g_uartMr, 0x07);
		set(g_uart1Base + g_uartCr, 0x04);
		set(g_uart1Base + g_uartBg2, 0x28);
		set(g_uart1Base + g_uartIvr, 0x40);
		set(g_uart2Base + g_uartMr, 0x07);
		set(g_uart2Base + g_uartCr, 0x04);
		set(g_uart2Base + g_uartBg2, 0x08);
		set(g_uart2Base + g_uartIvr, 0x40);
		set(g_ppddr, 0x01);
		set(g_ppdat, 0xfe);
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
			return computeParallelData();

		// UART status register (UM 12.4.1.3): computed, never the stored UCSR that a
		// write to this same offset leaves behind.
		if(_offset == g_uart1Base + g_uartUsr)	return computeUartStatus(g_uartMidi);
		if(_offset == g_uart2Base + g_uartUsr)	return computeUartStatus(g_uartPanel);

		// UART receiver buffer (UM 12.4.1.4 URB): pops the RX FIFO.
		if(_offset == g_uart1Base + g_uartRxTx)	return popReceiveBuffer(g_uartMidi);
		if(_offset == g_uart2Base + g_uartRxTx)	return popReceiveBuffer(g_uartPanel);

		// Timer counters (UM 14.4.1.4 TCN): a read yields the live counter, not a stored
		// value. TCN is 16-bit big-endian at base+$0C.
		if(_offset == g_timer1Base + g_timerTcn)		return static_cast<uint8_t>(m_timer[0].counter >> 8);
		if(_offset == g_timer1Base + g_timerTcn + 1)	return static_cast<uint8_t>(m_timer[0].counter);
		if(_offset == g_timer2Base + g_timerTcn)		return static_cast<uint8_t>(m_timer[1].counter >> 8);
		if(_offset == g_timer2Base + g_timerTcn + 1)	return static_cast<uint8_t>(m_timer[1].counter);

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

	void Sim::write8(const uint32_t _offset, const uint8_t _value)
	{
		if(_offset >= g_windowSize)
			return;

		// A register write can unmask or configure a source.  Be conservative here;
		// takeNextInterrupt() clears the gate after one full scan when the write was
		// unrelated to interrupt state.
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
			return;
		}
		if(_offset == g_timer2Base + g_timerTcn || _offset == g_timer2Base + g_timerTcn + 1)
		{
			m_timer[1] = Timer{};
			refreshTimerConfiguration(1, g_timer2Base);
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

		// UART interrupt-mask (UIMR, base+$14 on write): arm the transmitter-ready interrupt
		// when the firmware enables it (TxRDY). Our transmitter is always ready, so from here
		// the ISR is driven to drain the UART's transmit ring one byte per interrupt.
		if(_offset == g_uart1Base + g_uartIsr && (_value & g_uimrTxRdy))
			m_uartTxIrqArmed[g_uartMidi] = true;
		if(_offset == g_uart2Base + g_uartIsr && (_value & g_uimrTxRdy))
			m_uartTxIrqArmed[g_uartPanel] = true;

		// Default behaviour: every register (PPDDR/PPDAT latch, chip selects, timers,
		// UART mode/clock/command config, interrupt controller, ...) is stored so a
		// subsequent read returns what was written.
		m_mem[_offset] = _value;

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
		return static_cast<uint8_t>(
			(ddr & outLat) | (static_cast<uint8_t>(~ddr) & inputLevel));
	}

	// -------------------------------------------------------------------------
	// UART
	// -------------------------------------------------------------------------

	uint8_t Sim::computeUartStatus(const unsigned _uart) const
	{
		// UM 12.4.1.3 Status Register. We always advertise the transmitter as ready
		// (holding register empty AND fully drained) so the firmware's poll loop can
		// always send. Receiver flags follow the modelled RX FIFO.
		uint8_t usr = g_usrTxEmp | g_usrTxRdy;

		if(_uart < g_uartCount)
		{
			const auto& rx = m_uart[_uart].rx;
			if(!rx.empty())
				usr |= g_usrRxRdy;
			if(rx.size() >= 3)		// FIFO depth is 3 (UM 12.4.1.3 FFULL)
				usr |= g_usrFFull;
		}
		return usr;
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

		// Hardware: reading URB clears RxRDY; if the receiver still holds a byte it re-asserts
		// immediately, so the ISR is re-entered until the FIFO is drained. Re-arm the edge here
		// (rather than only in queueRx) so multi-byte streams - SysEx dumps, MIDI clock bursts -
		// are delivered in full instead of one byte per enqueue.
		if(!rx.empty())
		{
			const uint32_t base = (_uart == g_uartPanel) ? g_uart2Base : g_uart1Base;
			if(m_mem[base + g_uartIsr] & g_uimrRxRdy)
			{
				m_uartRxIrqArmed[_uart] = true;
				m_interruptCheckNeeded = true;
			}
		}
		return b;
	}

	void Sim::pushTransmitBuffer(const unsigned _uart, const uint8_t _value)
	{
		if(_uart >= g_uartCount)
			return;

		auto& u = m_uart[_uart];
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

		// If the receiver-ready interrupt is enabled, arm it so the firmware's UART RX ISR
		// runs and consumes the byte (this delivers panel button/encoder events at runtime).
		const uint32_t base = (_uart == g_uartPanel) ? g_uart2Base : g_uart1Base;
		if(m_mem[base + g_uartIsr] & g_uimrRxRdy)
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
		stepTimer(0, g_timer1Base, _cycles);
		stepTimer(1, g_timer2Base, _cycles);
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
		uint32_t ticksUntilMatch;
		if(!timer.freeRunning)
			ticksUntilMatch = timer.counter >= timer.period
				? 1 : timer.period - timer.counter;
		else
		{
			ticksUntilMatch =
				(static_cast<uint32_t>(timer.reference) - timer.counter) & 0xffff;
			if(!ticksUntilMatch)
				ticksUntilMatch = 0x10000;
		}
		return ticksUntilMatch * timer.div - timer.frac;
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
			matched = (total / timer.period) != 0;
			timer.counter = static_cast<uint16_t>(total % timer.period);
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
		// External IRQ4 is the DSP2 HI08 HREQ line (MCF5206E UM 8.3.2.3;
		// MAME Elektron driver). It is level-sensitive and uses the configured ICR.
		if(!m_extIrq4Level || imrMasked(g_irqSrcExtIrq4))
			return false;

		_level  = icrLevel(g_icrExtIrq4);
		_vector = static_cast<uint8_t>(24 + _level);	// external IRQ pins autovector (vector 24+IL)
		return true;
	}

	bool Sim::takeNextInterrupt(uint8_t& _level, uint8_t& _vector)
	{
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

			// Receiver-ready: a queued byte and the RX interrupt enabled. Consumed on inject, and
			// RE-ARMED when the ISR reads the RX register while another byte is still pending (see
			// popReceiveBuffer) - that mirrors the hardware, where reading URB clears RxRDY and the
			// next FIFO byte immediately re-asserts it. Without that re-arm only ONE byte per
			// queueRx was delivered: invisible for single-byte panel events, but it silently
			// truncated multi-byte streams (a 197-byte SysEx dump lost 196 bytes).
			// Checked first so input is serviced promptly.
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
