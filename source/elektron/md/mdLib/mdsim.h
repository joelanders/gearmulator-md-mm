#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace md
{
	// -------------------------------------------------------------------------
	// md::Sim - ColdFire MCF5206E System Integration Module (SIM) peripheral model.
	//
	// This models the on-chip peripheral register window described by the
	// Motorola/Freescale MCF5206E User's Manual (MCF5206EUM).
	//
	// mdmc maps the public MAME driver's 64 KB SIM window and calls this class
	// with MBAR-relative offsets.
	//
	// Modelled behaviourally (everything else is store/return-stored backing RAM):
	//   * Parallel port PPDDR/PPDAT (UM Section 10) - PPDAT reads return input pins
	//     idle-HIGH, output-driven bits reflect the last written value.
	//   * UART1 ($140, MIDI) and UART2 ($180, panel) status register (USR, +$04) -
	//     the transmitter is always reported ready (TxEMP/TxRDY) so the firmware can
	//     poll-and-send; RxRDY is set only while a byte is queued via queueRx().
	//     Transmit-buffer (UTB) writes are captured for later MIDI/panel wiring.
	//   * Chip-select, timer and interrupt-controller registers are stored so reads
	//     return what was written (they define the memory map, hard-coded elsewhere).
	//
	// The class carries NO gearmulator/JUCE/mc68k dependency and compiles standalone.
	// -------------------------------------------------------------------------
	class Sim
	{
	public:
		// Size of the modelled register window (MBAR-relative). UM maps the on-chip
		// peripherals into the space above MBAR; the MD decodes a 64 KB window.
		static constexpr uint32_t g_windowSize = 0x10000;

		// --- UM register-map offsets (MBAR-relative), cited per source table ------

		// SIM / interrupt controller (UM 8.3.1 Table "SIM Registers Memory Map").
		static constexpr uint32_t g_simr = 0x003;	// SIM Configuration Register,      8-bit, reset $C0
		static constexpr uint32_t g_icr1 = 0x014;	// Interrupt Control Reg 1..15,     8-bit each, $014..$022
		static constexpr uint32_t g_icrTimer1 = 0x01c;	// ICR9  = Timer 1 (UM 8.3.1: ICRn at $014+(n-1))
		static constexpr uint32_t g_icrTimer2 = 0x01d;	// ICR10 = Timer 2
		static constexpr uint32_t g_icrUart1  = 0x01f;	// ICR12 = UART1 (MIDI)
		static constexpr uint32_t g_icrUart2  = 0x020;	// ICR13 = UART2 (panel)
		static constexpr uint32_t g_icrExtIrq4= 0x017;	// ICR4  = external IRQ4 (ICRn at $014+(n-1); n=4).
												//   MD: DSP2 HI08 HREQ.
		static constexpr uint32_t g_imr  = 0x036;	// Interrupt Mask Register,        16-bit, reset $3FFE
		static constexpr uint32_t g_ipr  = 0x03a;	// Interrupt Pending Register,     16-bit, reset $0000, read-only

		// Interrupt-controller source bits in IMR/IPR (UM 8.3.2.4/8.3.2.5): the bit
		// index equals the ICR number, so Timer 1 = source 9, Timer 2 = source 10, and the
		// UARTs are 12 (UART1/MIDI) and 13 (UART2/panel).
		static constexpr uint32_t g_irqSrcTimer1 = 9;
		static constexpr uint32_t g_irqSrcTimer2 = 10;
		static constexpr uint32_t g_irqSrcUart1  = 12;
		static constexpr uint32_t g_irqSrcUart2  = 13;
		static constexpr uint32_t g_irqSrcExtIrq4= 4;	// external IRQ4 = source/ICR index 4 (DSP2 HREQ)

		// ICR bit 7 = AVEC (autovector). When clear the source supplies its own vector
		// (the UARTs use their UIVR); when set the ColdFire autovector 24+level is used.
		static constexpr uint8_t  g_icrAutovector = 0x80;

		// Chip selects (UM 9.4.2 Table 9-4). Bank n: CSAR=$64+12n, CSMR=$68+12n, CSCR=$6E+12n.
		static constexpr uint32_t g_csar0     = 0x064;	// Chip-Select Address Register 0, 16-bit
		static constexpr uint32_t g_csmr0     = 0x068;	// Chip-Select Mask Register 0,    32-bit
		static constexpr uint32_t g_cscr0     = 0x06e;	// Chip-Select Control Register 0, 16-bit
		static constexpr uint32_t g_csStride  = 0x00c;	// per-bank stride
		static constexpr uint32_t g_csBankMax = 7;		// banks 0..7

		// General-purpose timers (UM 14.4.1 Table 14-1). Timer2 = Timer1 + $20.
		static constexpr uint32_t g_timer1Base = 0x100;	// TMR1 $100, TRR1 $104, TCR1 $108, TCN1 $10C, TER1 $111
		static constexpr uint32_t g_timer2Base = 0x120;
		static constexpr uint32_t g_timerTmr   = 0x00;	// Timer Mode Register,      16-bit
		static constexpr uint32_t g_timerTrr   = 0x04;	// Timer Reference Register, 16-bit
		static constexpr uint32_t g_timerTcr   = 0x08;	// Timer Capture Register,   16-bit
		static constexpr uint32_t g_timerTcn   = 0x0c;	// Timer Counter,            16-bit
		static constexpr uint32_t g_timerTer   = 0x11;	// Timer Event Register,      8-bit

		// UART modules (UM 12.4.1 Table 12-1 "UART Module Programming Model").
		// UART1 base $140 (MD: MIDI), UART2 base $180 (MD: front panel).
		static constexpr uint32_t g_uart1Base = 0x140;
		static constexpr uint32_t g_uart2Base = 0x180;
		static constexpr uint32_t g_uartMr    = 0x00;	// R/W Mode Register (UMR1/UMR2)
		static constexpr uint32_t g_uartUsr   = 0x04;	// R: Status Register (USR)  / W: Clock-Select (UCSR)
		static constexpr uint32_t g_uartCr    = 0x08;	// W: Command Register (UCR)
		static constexpr uint32_t g_uartRxTx  = 0x0c;	// R: Receiver Buffer (URB)  / W: Transmit Buffer (UTB)
		static constexpr uint32_t g_uartIpcr  = 0x10;	// R: Input Port Change (UIPCR) / W: Aux Control (UACR)
		static constexpr uint32_t g_uartIsr   = 0x14;	// R: Interrupt Status (UISR)   / W: Interrupt Mask (UIMR)
		static constexpr uint32_t g_uartBg1   = 0x18;	// Baud-rate prescale MSB (UBG1)
		static constexpr uint32_t g_uartBg2   = 0x1c;	// Baud-rate prescale LSB (UBG2)
		static constexpr uint32_t g_uartIvr   = 0x30;	// Interrupt Vector Register (UIVR)

		// UIMR/UISR bits (UM 12.4.1.9). Bit 0 = transmitter-ready, bit 1 = receiver-ready.
		static constexpr uint8_t  g_uimrTxRdy = 0x01;
		static constexpr uint8_t  g_uimrRxRdy = 0x02;

		// TMR bit fields (UM 14.4.1.1 Timer Mode Register).
		static constexpr uint16_t g_tmrRst  = 0x0001;	// bit0    Reset/enable (1 = timer enabled)
		static constexpr uint16_t g_tmrIclk = 0x0006;	// bits2-1 Input clock (01 = master, 10 = master/16, 00 = stop)
		static constexpr uint16_t g_tmrFrr  = 0x0008;	// bit3    Free-run(0)/Restart(1)
		static constexpr uint16_t g_tmrOri  = 0x0010;	// bit4    Output-reference interrupt enable
		static constexpr uint16_t g_tmrPsShift = 8;		// bits15-8 Prescaler (divide by PS+1)

		// TER bit fields (UM 14.4.1.5 Timer Event Register). Write-1-to-clear.
		static constexpr uint8_t  g_terRef = 0x01;	// bit0 Output-reference event reached
		static constexpr uint8_t  g_terCap = 0x02;	// bit1 Capture event
		static constexpr uint32_t g_noTimerInterruptDeadline = ~uint32_t{0};

		// USR status bits (UM 12.4.1.3 Status Register). Read-only, reset 0.
		static constexpr uint8_t g_usrRxRdy = 0x01;	// bit0: receiver has a character
		static constexpr uint8_t g_usrFFull = 0x02;	// bit1: receive FIFO full (3 chars)
		static constexpr uint8_t g_usrTxRdy = 0x04;	// bit2: transmitter-holding register empty
		static constexpr uint8_t g_usrTxEmp = 0x08;	// bit3: transmitter completely empty (underrun)

		// Parallel port (UM 10.3.1 Table 10-1). 8-bit registers, reset $00.
		static constexpr uint32_t g_ppddr = 0x1c5;	// Port A Data Direction Register (0=input, 1=output)
		static constexpr uint32_t g_ppdat = 0x1c9;	// Port A Data Register

		// UART identifiers used by the accessors (index into the two UART modules).
		static constexpr unsigned g_uartCount = 2;
		static constexpr unsigned g_uartMidi  = 0;	// UART1 ($140)
		static constexpr unsigned g_uartPanel = 1;	// UART2 ($180)
		// Host-side scheduling backlog, not the physical three-byte FIFO. Fixed
		// storage removes allocator work from emulation while allowing large bursts.
		static constexpr size_t g_uartRxCapacity = 65536;
		static constexpr size_t g_uartTxCapacity = 4096;
		// Callback invoked for every byte the firmware writes to a UART transmit
		// buffer (UTB). Used later to route MIDI (UART1) / panel (UART2) traffic.
		using TransmitCallback = std::function<void(uint8_t)>;

		Sim();

		// Return every register to its documented power-on state. Transmit callbacks
		// (which are wiring, not device state) are preserved.
		void reset();

		// -------------------------------------------------------------------------
		// Register access entry points. mdmc resolves an address in $300000..$30ffff
		// to the MBAR-relative offset (address - $300000) and calls these. Big-endian
		// (68k/ColdFire) byte order, matching the rest of the MD memory model. Reads
		// are non-const because reading a UART receive buffer pops the RX FIFO.
		// -------------------------------------------------------------------------
		uint8_t  read8 (uint32_t _offset);
		uint16_t read16(uint32_t _offset);
		uint32_t read32(uint32_t _offset);

		void write8 (uint32_t _offset, uint8_t  _value);
		void write16(uint32_t _offset, uint16_t _value);
		void write32(uint32_t _offset, uint32_t _value);

		// --- Parallel port helpers -----------------------------------------------

		// Logic level presented on pins configured as inputs (UM 10.3.2.2). The MD
		// wiring is idle-high; bits configured as outputs ignore this value.
		void    setParallelInputLevel(uint8_t _level) { m_ppInputLevel = _level; }
		uint8_t getParallelInputLevel() const         { return m_ppInputLevel; }
		void setMk2PortAInvertedLoopback(bool _enabled)
		{
			m_mk2PortAInvertedLoopback = _enabled;
		}

		uint8_t getParallelDirection() const { return m_mem[g_ppddr]; }	// PPDDR
		uint8_t getParallelData()      const { return computeParallelData(); }	// value a PPDAT read returns

		// --- UART helpers ---------------------------------------------------------

		// Register a sink for bytes the firmware transmits on this UART (UTB writes).
		void setTransmitCallback(unsigned _uart, TransmitCallback _callback);

		// tryQueueRx rejects the newest byte when full and records an overflow.
		// queueRx is retained for compatibility; lossless callers must use tryQueueRx.
		bool tryQueueRx(unsigned _uart, uint8_t _byte);
		void queueRx(unsigned _uart, uint8_t _byte);
		bool hasQueuedRx(unsigned _uart) const;
		size_t queuedRxBytes(unsigned _uart) const;
		size_t availableRxBytes(unsigned _uart) const;
		size_t rxOverflowCount(unsigned _uart) const;
		uint64_t rxConsumedCount(unsigned _uart) const;
		bool isReceiveInterruptEnabled(unsigned _uart) const
		{
			if(_uart >= g_uartCount)
				return false;
			const auto base = _uart == g_uartPanel ? g_uart2Base : g_uart1Base;
			return (m_mem[base + g_uartIsr] & g_uimrRxRdy) != 0;
		}

		// --- Timers + interrupt controller (UM 14.4 / 8.3.2.3-8.3.2.5) ------------

		// Advance the on-chip general-purpose timers by _cycles master-clock cycles.
		// An enabled timer (TMR RST=1, ICLK!=stop) counts its (prescaled) clock toward
		// the 16-bit reference (TRR); on a reference match the TER REF bit is set. In
		// free-run mode (TMR FRR=0, as the MD programs Timer 1) the counter continues to
		// 0xffff and wraps, so REF recurs once per full 0x10000-count cycle. With ORI set
		// and the source unmasked in IMR this asserts the timer interrupt (level/vector
		// from the timer's ICR - the MD's tick is Timer 1: autovectored, level 1).
		void exec(uint32_t _cycles);
		bool needsInterruptCheck() const { return m_interruptCheckNeeded; }
		bool externalIrq4Asserted() const { return m_extIrq4Level; }

		// Number of master-clock cycles until exec() can first make a new, currently
		// unmasked timer interrupt injectable. Zero means a REF event is already
		// latched and has not yet been injected; g_noTimerInterruptDeadline means
		// neither timer can publish a new interrupt without a register write.
		//
		// A bounded CPU fast path may advance through this many cycles only when it
		// materializes state and calls exec()/takeNextInterrupt immediately after the
		// instruction that reaches the deadline. It must not execute a later
		// instruction first.
		uint32_t cyclesUntilNextTimerInterrupt() const;

		// Interrupt-controller: hand the next pending interrupt to the parent CPU and
		// consume it, returning false when none is pending above the mask. Sources modelled:
		//   * Timers 1/2 (level, autovectored) - REF set AND ORI AND source unmasked; the
		//     latch re-arms only after the ISR clears TER (edge tracked internally).
		//   * UART1/2 transmitter-ready (edge, programmed vector via UIVR) - UIMR TxRDY
		//     enabled AND source unmasked; re-armed on each UTB write so the ISR drains its
		//     transmit ring one byte per interrupt (this is how the panel/LCD stream flows).
		// _level/_vector receive the interrupt to inject; higher-priority sources first.
		bool takeNextInterrupt(uint8_t& _level, uint8_t& _vector);

		// Drive the external IRQ4 input level (UM 8.3.2.3 external interrupt sources). On the MD
		// this pin is wired to DSP2's HI08 HREQ output (MAME elektronmono.cpp: m_dsp2gm->irq_cb()
		// -> write_irq_4). Level-sensitive - it stays asserted as long as DSP2 has words for the
		// host; the handler silences it by draining DSP2's HI08. See getExternalIrq4.
		void setExternalIrq4(bool _level);

		// Query the external IRQ4 line for the CPU. Returns true (and fills the autovector _level /
		// _vector) while the line is asserted AND unmasked in IMR. UNLIKE the timer/UART edge
		// sources this is NOT self-consuming: the caller (mdmc) re-offers it to the CPU while the
		// line stays high, gated by its IRQ4 pending state, so the handler re-fires
		// whenever HREQ remains high after it returns.
		bool getExternalIrq4(uint8_t& _level, uint8_t& _vector);

		// --- MBAR (optional) ------------------------------------------------------
		// The Sim operates on MBAR-relative offsets but retains the raw register value
		// so callers can query the decoded base.
		void     setMbar(uint32_t _mbar)  { m_mbar = _mbar; }
		uint32_t getMbar() const          { return m_mbar; }
		uint32_t getMbarBase() const      { return m_mbar & ~uint32_t(0xfff); }	// strip V bit / low control bits

	private:
		template<size_t Capacity>
		struct ByteQueue
		{
			bool push(const uint8_t _value)
			{
				if(count == Capacity)
					return false;
				bytes[(read + count) % Capacity] = _value;
				++count;
				return true;
			}

			bool pop(uint8_t& _value)
			{
				if(count == 0)
					return false;
				_value = bytes[read];
				read = (read + 1) % Capacity;
				--count;
				return true;
			}

			void clear() { read = 0; count = 0; }
			bool empty() const { return count == 0; }
			size_t size() const { return count; }
			size_t available() const { return Capacity - count; }

			std::array<uint8_t, Capacity> bytes{};
			size_t read = 0;
			size_t count = 0;
		};

		struct Uart
		{
			ByteQueue<g_uartRxCapacity> rx;	// bytes waiting to be read from URB
			TransmitCallback    txCallback;
			size_t rxOverflows = 0;
			uint64_t rxConsumed = 0;
		};

		uint8_t computeParallelData() const;
		uint8_t computeUartStatus(unsigned _uart) const;
		uint8_t popReceiveBuffer(unsigned _uart);
		void    pushTransmitBuffer(unsigned _uart, uint8_t _value);

		// --- Timer internals ------------------------------------------------------
		struct Timer
		{
			uint16_t counter = 0;	// live TCN (a TCN read returns this; a write resets it to 0)
			uint32_t frac    = 0;	// prescaler/clock-divider remainder not yet applied to counter
			// Decoded TMR/TRR state. Firmware changes these registers rarely, while exec()
			// runs after every ColdFire instruction, so keep the hot path free of repeated
			// big-endian register assembly and bit-field decoding.
			uint32_t div = 1;
			uint32_t period = 1;
			uint16_t reference = 0;
			bool running = false;
			bool freeRunning = false;
		};

		void     stepTimer(unsigned _index, uint32_t _base, uint32_t _cycles);
		void     refreshTimerConfiguration(unsigned _index, uint32_t _base);
		uint32_t cyclesUntilTimerInterrupt(
			unsigned _index, uint32_t _base, uint32_t _sourceBit) const;
		bool     timerAssertsIrq(uint32_t _base) const;	// TER REF && TMR ORI (IRQ line to the controller)
		uint16_t reg16(uint32_t _offset) const;			// big-endian 16-bit read of the backing store
		bool     imrMasked(uint32_t _sourceBit) const;	// IMR bit for an interrupt source
		uint8_t  icrLevel(uint32_t _icrOffset) const;	// interrupt level from an ICR (bits 4-2)

		// Backing store for the whole window. Big-endian natural order, so 16/32-bit
		// accesses reassemble from bytes exactly like the MD RAM/ROM regions.
		std::array<uint8_t, g_windowSize> m_mem{};

		// Level on input-configured parallel-port pins (idle-high on the MD).
		uint8_t m_ppInputLevel = 0xff;
		// MKII motherboard strap: input bit 0 inverts driven output bit 2.
		bool m_mk2PortAInvertedLoopback = false;

		std::array<Uart, g_uartCount> m_uart{};

		std::array<Timer, 2> m_timer{};	// Timer 1 ($100) and Timer 2 ($120)

		// Interrupt edge latches. Timer: set true once the pending IRQ has been injected,
		// re-armed (cleared) when the ISR clears TER. UART TX: set true when the transmitter
		// wants an interrupt (UIMR enabled / a UTB write), cleared when injected.
		std::array<bool, 2> m_timerIrqInjected{};	// Timer 1 / Timer 2
		std::array<bool, 2> m_uartTxIrqArmed{};		// UART1 / UART2 transmitter-ready
		std::array<bool, 2> m_uartRxIrqArmed{};		// UART1 / UART2 receiver-ready (a byte was queued)
		// Most ColdFire instructions cannot create a SIM interrupt.  This conservative
		// gate is raised by every source/configuration transition and cleared only after
		// a complete priority scan finds no injectable source.  It therefore removes the
		// settled per-instruction UART/timer scan without changing interrupt boundaries.
		bool m_interruptCheckNeeded = false;

		// External IRQ4 (DSP2 HI08 HREQ). Pure level line; the CPU's own pending-interrupt queue
		// (mdmc) provides the "don't re-offer while already pending" gate, so no edge latch here.
		bool m_extIrq4Level  = false;	// current logic level on the external IRQ4 pin

		uint32_t m_mbar = 0x00300001;	// MD default: base $300000, V=1
	};
}
