#include "../mdLib/mdsim.h"
#include "mc68k/mc68k.h"
#include "mc68k/cpuState.h"
// Use base-class memory callbacks for this synthetic CPU, not mdLib's
// specialized callbacks that require an actual md::Microcontroller instance.
#include "mc68k/musashiEntry.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	class Cpu final : public mc68k::Mc68k
	{
	public:
		Cpu() : Mc68k(M68K_CPU_TYPE_MCF5206E)
		{
			write16(0, 0);
			write16(2, 0xf00); // Initial supervisor stack.
			write16(4, 0);
			write16(6, 0x200); // Reset PC.
			// Entirely synthetic: spin in main, set D0 in the interrupt handler.
			write16(0x200, 0x60fe); // bra.s *
			write16(0x400, 0x705a); // moveq #$5a,d0
			write16(0x402, 0x60fe); // bra.s *
			write16(0x60 * 4, 0);
			write16(0x60 * 4 + 2, 0x400);
			reset();
			m68k_set_reg(getCpuState(), M68K_REG_D0, 0);
		}

		uint32_t exec() override { return execInstruction(); }

		uint8_t read8(const uint32_t _address) override
		{
			if(_address >= m_memory.size())
				throw std::runtime_error("synthetic read outside RAM: " + std::to_string(_address));
			return m_memory[_address];
		}

		uint16_t read16(const uint32_t _address) override
		{
			return (uint16_t(read8(_address)) << 8) | read8(_address + 1);
		}

		uint16_t readImm16(const uint32_t _address) override { return read16(_address); }

		void write8(const uint32_t _address, const uint8_t _value) override
		{
			if(_address >= m_memory.size())
				throw std::runtime_error("synthetic write outside RAM: " + std::to_string(_address));
			m_memory[_address] = _value;
		}

		void write16(const uint32_t _address, const uint16_t _value) override
		{
			write8(_address, static_cast<uint8_t>(_value >> 8));
			write8(_address + 1, static_cast<uint8_t>(_value));
		}

	private:
		std::array<uint8_t, 4096> m_memory{};
	};
}

namespace
{
	enum class PendingChange { None, SameMask, UartMaskToggle, GlobalMaskToggle, AppendByte };

	void pendingReceive(const unsigned _uart, const PendingChange _change)
	{
		Cpu cpu;
		md::Sim sim;
		const auto base = _uart == md::Sim::g_uartPanel ? md::Sim::g_uart2Base : md::Sim::g_uart1Base;
		const auto icr = _uart == md::Sim::g_uartPanel ? md::Sim::g_icrUart2 : md::Sim::g_icrUart1;
		sim.write16(md::Sim::g_imr, 0);
		sim.write8(icr, 3 << 2);
		sim.write8(base + md::Sim::g_uartMr, 0x13);
		sim.write8(base + md::Sim::g_uartCr, 0x05);
		sim.write8(base + md::Sim::g_uartIvr, 0x60);
		sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
		sim.queueRx(_uart, 0x42);
		uint8_t level = 0, vector = 0;
		require(sim.takeNextInterrupt(level, vector), "UART did not offer its receive request");
		require(level == 3 && vector == 0x60, "UART offered the wrong level/vector");
		cpu.injectInterrupt(vector, level);
		// Match the production handoff: consume each SIM offer and inject it,
		// without hiding duplicates behind a test-only CPU deduplication guard.
		const auto deliver = [&]()
		{
			while(sim.takeNextInterrupt(level, vector))
				cpu.injectInterrupt(vector, level);
		};
		for(unsigned i = 0; i < 3; ++i)
		{
			if(_change == PendingChange::SameMask)
				sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
			else if(_change == PendingChange::UartMaskToggle)
			{
				sim.write8(base + md::Sim::g_uartIsr, 0);
				deliver();
				sim.write8(base + md::Sim::g_uartIsr, md::Sim::g_uimrRxRdy);
			}
			else if(_change == PendingChange::GlobalMaskToggle)
			{
				sim.write16(md::Sim::g_imr, 0x3ffe);
				deliver();
				sim.write16(md::Sim::g_imr, 0);
			}
			else if(_change == PendingChange::AppendByte && i == 0)
				sim.queueRx(_uart, 0x43);
			deliver();
		}
		for(unsigned i = 0; i < 16; ++i)
			cpu.exec();
		require(cpu.getDReg(0) == 0, "CPU serviced UART despite reset interrupt mask");
		require(cpu.hasPendingInterrupt(0x60, 3), "CPU masking lost the offered UART request");
		require(!sim.takeNextInterrupt(level, vector), "fixture unexpectedly offered a second UART event");
		m68k_set_reg(cpu.getCpuState(), M68K_REG_SR, 0x2000);
		for(unsigned i = 0; i < 16; ++i)
			cpu.exec();
		require(cpu.getDReg(0) == 0x5a, "pending UART request was not serviced after CPU unmask");
		const bool appended = _change == PendingChange::AppendByte;
		require(sim.queuedRxBytes(_uart) == (appended ? 2u : 1u), "interrupt acknowledge consumed UART data");
		require(sim.read8(base + md::Sim::g_uartRxTx) == 0x42, "CPU masking changed receive data");
		require(!cpu.hasPendingInterrupt(0x60, 3), "duplicate UART request remained queued after RX service");
		if(appended)
		{
			require(sim.takeNextInterrupt(level, vector), "next FIFO byte did not rearm RX after service");
			require(!sim.takeNextInterrupt(level, vector), "next FIFO byte offered more than one RX request");
			require(sim.read8(base + md::Sim::g_uartRxTx) == 0x43, "appended receive data changed");
		}
		require(!sim.takeNextInterrupt(level, vector), "drained UART requested extra service");
	}
}

int main()
{
	unsigned failures = 0;
	for(unsigned uart = 0; uart < md::Sim::g_uartCount; ++uart)
	{
		for(const auto change : {PendingChange::None, PendingChange::SameMask, PendingChange::UartMaskToggle,
			PendingChange::GlobalMaskToggle, PendingChange::AppendByte})
		{
			try
			{
				pendingReceive(uart, change);
			}
			catch(const std::exception& error)
			{
				std::cerr << "UART " << uart << " case " << static_cast<unsigned>(change) << ": " << error.what() << '\n';
				++failures;
			}
		}
	}
	if(failures)
		return 1;
	std::cout << "UART requests survive CPU masking without duplicate delivery on both ports\n";
	return 0;
}
