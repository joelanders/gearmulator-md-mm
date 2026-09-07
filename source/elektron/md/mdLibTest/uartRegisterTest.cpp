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
		sim.write8(base + md::Sim::g_uartIsr, 0xff);
		require(sim.read8(base + md::Sim::g_uartIsr) == ready, "UIMR write changed source status");
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
		const auto icr = _uart == md::Sim::g_uartPanel ? md::Sim::g_icrUart2 : md::Sim::g_icrUart1;
		sim.write8(icr, 3 << 2);
		sim.write8(base + md::Sim::g_uartIvr, 0x60);
		sim.write16(md::Sim::g_imr, 0);
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
}

int main()
{
	unsigned failures = 0;
	for(unsigned uart = 0; uart < md::Sim::g_uartCount; ++uart)
	{
		// Run independently so a status failure cannot hide the unmasking regression.
		for(const auto test : {sourceStatus, receiveUnmask})
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
