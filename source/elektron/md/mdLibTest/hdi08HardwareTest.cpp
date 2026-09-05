#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
	struct Fixture
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Peripherals56303 peripherals;
		dsp56k::PeripheralsNop unused;
		dsp56k::Memory memory{validator, 0x10000, 0x10000, 0x8000};
		dsp56k::DSP dsp{memory, &peripherals, &unused};
		Fixture()
		{
			dsp56k::Assembler assembler;
			const auto emit = [&](uint32_t address, const char* instruction)
			{
				const auto assembled = assembler.assemble(instruction);
				if(!assembled.success()) throw std::runtime_error("synthetic assembly failed");
				for(unsigned i = 0; i < assembled.wordCount; ++i)
					dsp.memWriteP(address + i, assembled.word[i]);
				return address + assembled.wordCount;
			};
			emit(0x100, "jmp $100");
			emit(0x20, "jsr $200");
			const auto end = emit(0x200, "move #$5a,x0");
			emit(end, "rti");
			dsp.regs().sr.var = 0;
			dsp.setPC(0x100);
		}
		void advance() { dsp.execUntilCycles(dsp.getCycles() + 4096); }
		// The short immediate MOVE to X0 places its byte in bits 23:16.
		bool handled() { return dsp.x0().var == 0x5a0000; }
	};

	void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}
}

int main()
{
	try
	{
		{
			auto enabled = std::make_unique<Fixture>();
			auto& port = enabled->peripherals.getHI08();
			port.setHostCommandArbitration(true);
			port.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			port.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
			require(!enabled->dsp.hasPendingInterrupts(), "enabled fixture starts with an interrupt");
			port.writeHostCommand(0x20);
			enabled->advance();
			require(enabled->handled(), "enabled host command did not execute its handler");
			require(!(port.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP)),
				"serviced host command retained HCP");
		}
		// DSP56303UM 6.6.1 / table 6-8: HCIE gates the host-command interrupt,
		// not the pending status. This uses no firmware, private RAM, or payload.
		auto fixture = std::make_unique<Fixture>();
		auto& port = fixture->peripherals.getHI08();
		port.setHostCommandArbitration(true);
		port.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
		port.writeControlRegister(0);
		require(!fixture->dsp.hasPendingInterrupts(), "fixture starts with an interrupt");
		port.writeHostCommand(0x20);
		require(port.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP),
			"disabled host command did not retain HCP");
		fixture->advance();
		require(!fixture->handled(), "HCIE=0 still executed the host interrupt handler");
		require(port.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP),
			"disabled command lost HCP before service");
		port.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
		fixture->advance();
		require(fixture->handled(), "enabling HCIE lost the pending command");
		std::cout << "HI08 hardware control: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "HI08 hardware control: " << error.what() << '\n';
		return 1;
	}
}
