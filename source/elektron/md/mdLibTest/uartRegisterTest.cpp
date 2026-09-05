#include "mdLib/mdsim.h"

#include <iostream>
#include <stdexcept>

namespace
{
	void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}
}

int main()
{
	try
	{
		for(unsigned uart = 0; uart < md::Sim::g_uartCount; ++uart)
		{
			md::Sim sim;
			const auto base = uart == md::Sim::g_uartPanel ? md::Sim::g_uart2Base : md::Sim::g_uart1Base;
			// MCF5206EUM 12.4.1.10/.11: UIMR affects interrupt delivery, not UISR reads.
			// Select receive-ready mode and enable RX/TX. This is not a test of the
			// still-incomplete UART command, mode-pointer, or serial timing model.
			sim.write8(base + md::Sim::g_uartMr, 0x13);
			sim.write8(base + md::Sim::g_uartCr, 0x05);
			sim.write8(base + md::Sim::g_uartIsr, 0);
			sim.queueRx(uart, 0x42);
			const auto ready = sim.read8(base + md::Sim::g_uartIsr);
			require(ready & md::Sim::g_uimrRxRdy, "masked receive readiness disappeared from UISR");
			require(!sim.isReceiveInterruptEnabled(uart), "status read changed UIMR");
			require(sim.queuedRxBytes(uart) == 1, "status read consumed receive data");
			sim.write8(base + md::Sim::g_uartIsr, 0xff);
			require(sim.read8(base + md::Sim::g_uartIsr) == ready, "UIMR write changed source status");
			require(sim.isReceiveInterruptEnabled(uart), "UIMR write did not enable receive interrupts");
			require(sim.read8(base + md::Sim::g_uartRxTx) == 0x42, "receive data changed");
			require(!(sim.read8(base + md::Sim::g_uartIsr) & md::Sim::g_uimrRxRdy),
				"enabled receive interrupt reported data after FIFO drained");
			require(sim.isReceiveInterruptEnabled(uart), "draining receive data changed UIMR");

			// A byte received while masked must request service when RX is enabled;
			// no second incoming byte or transmit-ready interrupt may be required.
			sim.write8(base + md::Sim::g_uartIsr, 0);
			sim.queueRx(uart, 0x57);
			const auto icr = uart == md::Sim::g_uartPanel ? md::Sim::g_icrUart2 : md::Sim::g_icrUart1;
			sim.write8(icr, 3 << 2);
			sim.write8(base + md::Sim::g_uartIvr, 0x60);
			sim.write16(md::Sim::g_imr, 0);
			uint8_t level = 0, vector = 0;
			require(!sim.takeNextInterrupt(level, vector), "masked UART requested service");
			sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
			require(sim.takeNextInterrupt(level, vector), "unmasking stranded an already-received byte");
			require(level == 3 && vector == 0x60, "receive request used the wrong level/vector");
			require(sim.read8(base + md::Sim::g_uartRxTx) == 0x57, "unmasking changed pending receive data");
			require(!sim.takeNextInterrupt(level, vector), "drained receiver requested extra service");
		}
		std::cout << "UART status/mask separation passed for both ports\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
