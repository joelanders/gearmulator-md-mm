#include "mdLib/mdsim.h"

#include <iostream>
#include <stdexcept>

namespace
{
	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	uint32_t configureUart(md::Sim& _sim, const unsigned _uart)
	{
		const auto base = _uart == md::Sim::g_uartPanel ? md::Sim::g_uart2Base : md::Sim::g_uart1Base;
		// Select receive-ready mode and enable RX/TX. This does not validate the
		// incomplete command, mode-pointer, FIFO-full interrupt or serial timing model.
		_sim.write8(base + md::Sim::g_uartMr, 0x13);
		_sim.write8(base + md::Sim::g_uartCr, 0x05);
		const auto icr = _uart == md::Sim::g_uartPanel ? md::Sim::g_icrUart2 : md::Sim::g_icrUart1;
		_sim.write8(icr, 3 << 2);
		_sim.write8(base + md::Sim::g_uartIvr, 0x60);
		_sim.write16(md::Sim::g_imr, 0);
		return base;
	}

	void sourceStatus(const unsigned _uart)
	{
		md::Sim sim;
		const auto base = configureUart(sim, _uart);
		// MCF5206EUM 12.4.1.10/.11: UIMR gates delivery, not UISR source status.
		sim.write8(base + md::Sim::g_uartIsr, 0);
		require(sim.read8(base + md::Sim::g_uartIsr) == md::Sim::g_uimrTxRdy,
			"masked transmitter readiness disappeared from UISR");
		sim.queueRx(_uart, 0x42);
		const auto ready = sim.read8(base + md::Sim::g_uartIsr);
		require(ready == (md::Sim::g_uimrTxRdy | md::Sim::g_uimrRxRdy), "incorrect ready sources in UISR");
		require(!sim.isReceiveInterruptEnabled(_uart), "status read changed UIMR");
		require(sim.queuedRxBytes(_uart) == 1, "status read consumed receive data");
		const auto other = _uart == md::Sim::g_uartMidi ? md::Sim::g_uartPanel : md::Sim::g_uartMidi;
		const auto otherBase = other == md::Sim::g_uartPanel ? md::Sim::g_uart2Base : md::Sim::g_uart1Base;
		for(unsigned mask = 0; mask < 256; ++mask)
		{
			sim.write8(base + md::Sim::g_uartIsr, static_cast<uint8_t>(mask));
			require(sim.read8(base + md::Sim::g_uartIsr) == ready, "UIMR write changed source status");
			require(sim.isReceiveInterruptEnabled(_uart) == ((mask & md::Sim::g_uimrRxRdy) != 0),
				"status read changed UIMR");
			require(!sim.isReceiveInterruptEnabled(other), "mask write affected the other UART");
			require(sim.read8(otherBase + md::Sim::g_uartIsr) == md::Sim::g_uimrTxRdy,
				"receive status leaked to the other UART");
		}
		require(sim.queuedRxBytes(_uart) == 1, "repeated status reads consumed receive data");
		require(sim.isReceiveInterruptEnabled(_uart), "UIMR write did not enable receive interrupts");
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x42, "receive data changed");
		require(sim.read8(base + md::Sim::g_uartIsr) == md::Sim::g_uimrTxRdy,
			"drained receiver still reported ready");
		require(sim.isReceiveInterruptEnabled(_uart), "draining receive data changed UIMR");
	}

	void receiveUnmask(const unsigned _uart)
	{
		md::Sim sim;
		const auto base = configureUart(sim, _uart);
		sim.write8(base + md::Sim::g_uartIsr, 0);
		sim.queueRx(_uart, 0x57);
		uint8_t level = 0, vector = 0;
		require(!sim.takeNextInterrupt(level, vector), "masked UART requested service");
		require(!sim.needsInterruptCheck(), "idle interrupt scan did not clear the check gate");
		// No new byte or TX-ready interrupt may be needed to service pending RX.
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		require(sim.needsInterruptCheck(), "unmasking did not schedule an interrupt check");
		require(sim.takeNextInterrupt(level, vector), "unmasking stranded an already-received byte");
		require(level == 3 && vector == 0x60, "receive request used the wrong level/vector");
		require(sim.queuedRxBytes(_uart) == 1, "interrupt offer consumed receive data");
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x57, "unmasking changed pending receive data");
		require(!sim.takeNextInterrupt(level, vector), "drained receiver requested extra service");
	}

	void pendingMaskChanges(const unsigned _uart)
	{
		md::Sim sim;
		const auto base = configureUart(sim, _uart);
		const auto source = _uart == md::Sim::g_uartPanel ? md::Sim::g_irqSrcUart2 : md::Sim::g_irqSrcUart1;
		sim.write16(md::Sim::g_imr, static_cast<uint16_t>(1u << source));
		sim.queueRx(_uart, 0x42);
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		uint8_t level = 0, vector = 0;
		require(!sim.takeNextInterrupt(level, vector), "global mask did not block RX");
		require(!sim.needsInterruptCheck(), "globally masked scan did not settle");
		sim.write16(md::Sim::g_imr, 0);
		require(sim.needsInterruptCheck(), "global unmask did not reactivate the check gate");
		require(sim.takeNextInterrupt(level, vector), "global unmask lost pending RX");
		require(level == 3 && vector == 0x60, "global unmask changed interrupt routing");

		// None of these writes creates new RX data or consumes the offered byte.
		// Bit 7 changes another mask bit without enabling the model's TX source.
		for(const uint8_t mask : {0x02, 0x82, 0x80, 0x82, 0x00, 0x02, 0x02})
		{
			sim.write8(base + md::Sim::g_uartIsr, mask);
			require(!sim.takeNextInterrupt(level, vector), "mask write duplicated an offered RX request");
			require(!sim.needsInterruptCheck(), "mask-only scan did not settle");
		}
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x42, "mask changes lost the offered byte");
		sim.queueRx(_uart, 0x43);
		require(sim.takeNextInterrupt(level, vector), "fresh receive data did not rearm RX");
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x43, "fresh receive data changed");
		require(!sim.takeNextInterrupt(level, vector), "freshly drained RX requested extra service");
	}

	void maskedDrain(const unsigned _uart)
	{
		md::Sim sim;
		const auto base = configureUart(sim, _uart);
		sim.queueRx(_uart, 0x41);
		sim.queueRx(_uart, 0x42);
		sim.queueRx(_uart, 0x43);
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x41, "masked polling changed the first byte");
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		uint8_t level = 0, vector = 0;
		require(sim.takeNextInterrupt(level, vector), "partially drained RX was stranded on unmask");
		require(!sim.takeNextInterrupt(level, vector), "queued bytes duplicated the current RX offer");
		sim.write8(base + md::Sim::g_uartIsr, 0);
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x42, "masked drain changed the second byte");
		require(!sim.takeNextInterrupt(level, vector), "draining bypassed the UART mask");
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		require(sim.takeNextInterrupt(level, vector), "masked drain lost the next byte's RX request");
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x43, "masked drain changed the last byte");
		require(!sim.takeNextInterrupt(level, vector), "fully drained RX requested extra service");

		// Draining before unmask must not resurrect a stale receive request.
		sim.write8(base + md::Sim::g_uartIsr, 0);
		sim.queueRx(_uart, 0x44);
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x44, "masked polling changed fresh data");
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		require(!sim.takeNextInterrupt(level, vector), "unmask resurrected drained RX");
		require(!sim.needsInterruptCheck(), "drained RX left the interrupt-check gate hot");

		sim.queueRx(_uart, 0x45);
		sim.reset();
		configureUart(sim, _uart);
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		require(sim.queuedRxBytes(_uart) == 0, "reset retained receive data");
		require(!sim.takeNextInterrupt(level, vector), "reset retained a receive request");
	}
}

int main()
{
	unsigned failures = 0;
	for(unsigned uart = 0; uart < md::Sim::g_uartCount; ++uart)
	{
		// Run independently so a status failure cannot hide the unmasking regression.
		for(const auto test : {sourceStatus, receiveUnmask, pendingMaskChanges, maskedDrain})
		{
			try
			{
				test(uart);
			}
			catch(const std::exception& error)
			{
				std::cerr << "UART " << uart << ": " << error.what() << '\n';
				++failures;
			}
		}
	}
	if(failures)
		return 1;
	std::cout << "UART source-status and receive-unmask tests passed for both ports\n";
	return 0;
}
