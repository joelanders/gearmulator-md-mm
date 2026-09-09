#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "mc68k/hdi08.h"

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
			auto handler = 0x200u;
			const auto end = emit(handler, "move #$5a,x0");
			emit(end, "rti");
			emit(0x22, "jsr $400");
			emit(emit(0x400, "move #$33,y0"), "rti");
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
		for(const bool cancel : {false, true})
		{
			auto bridge = std::make_unique<Fixture>();
			auto& dspPort = bridge->peripherals.getHI08();
			mc68k::Hdi08 host;
			dspPort.setHostCommandArbitration(true);
			dspPort.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			host.setWriteIrqCallback([&](uint8_t vector) { dspPort.writeHostCommand(vector); });
			host.setHostCommandCallbacks([&] { return dspPort.hostCommandPending(); },
				[&] { dspPort.cancelHostCommand(); });
			host.write8(mc68k::PeriphAddress::HdiCVR, mc68k::Hdi08::Hc | 0x10);
			bridge->advance(); // HCIE remains disabled.
			require(host.read8(mc68k::PeriphAddress::HdiCVR) == (mc68k::Hdi08::Hc | 0x10),
				"host HC cleared before DSP acceptance or changed HV");
			bridge->dsp.regs().sr.var = 0x300;
			dspPort.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
			bridge->advance();
			require(bridge->dsp.hasPendingInterrupts(), "bridge did not queue a masked command");
			if(cancel) host.write8(mc68k::PeriphAddress::HdiCVR, 0x10);
			bridge->dsp.regs().sr.var = 0;
			bridge->advance();
			require(bridge->handled() != cancel, "host cancellation/acceptance delivered the wrong handler");
			require(host.read8(mc68k::PeriphAddress::HdiCVR) == 0x10,
				"host HC survived acceptance/cancellation");
			require(!dspPort.hostCommandPending(), "DSP HCP survived acceptance/cancellation");
		}
		std::cout << "HI08 host bridge: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "HI08 host bridge: " << error.what() << '\n';
		return 1;
	}
}
