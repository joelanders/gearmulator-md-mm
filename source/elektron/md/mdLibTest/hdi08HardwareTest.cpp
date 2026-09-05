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
			const auto end = emit(0x200, "move #$5a,x0");
			emit(end, "rti");
			emit(0x22, "jsr $210");
			emit(emit(0x210, "move #$33,y0"), "rti");
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
			dsp56k::PeripheralsNop schedule;
			schedule.resetDelayCycles(100, 100);
			require(!schedule.isDue(100, 0), "future peripheral tick is already due");
			schedule.requestExec();
			require(schedule.isDue(100, 0), "host wake did not make peripherals due");
			schedule.resetDelayCycles(100, 100);
			require(schedule.isDue(100, 0), "rescheduling lost an unconsumed host wake");
			schedule.beginExec();
			schedule.resetDelayCycles(100, 100);
			require(!schedule.isDue(100, 0), "consumed wake prevented rescheduling");
			schedule.beginExec();
			schedule.requestExec();
			schedule.resetDelayCycles(100, 100);
			require(schedule.isDue(100, 0), "wake during peripheral execution was lost");
		}
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

		// Withdraw an already queued source without blocking a different vector.
		auto queued = std::make_unique<Fixture>();
		auto& queuedPort = queued->peripherals.getHI08();
		queuedPort.setHostCommandArbitration(true);
		queuedPort.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
		queuedPort.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
		queued->dsp.regs().sr.var = 0x300; // IPL 3: enqueue without accepting the request.
		queuedPort.writeHostCommand(0x20);
		queued->advance();
		require(queued->dsp.hasPendingInterrupts() && !queued->handled(),
			"queued-command fixture did not retain a masked CPU request");
		queuedPort.writeControlRegister(0);
		queued->dsp.regs().sr.var = 0;
		queued->dsp.injectExternalInterrupt(0x22);
		queued->advance();
		require(!queued->handled(), "disabling a queued command did not prevent service");
		require(queued->dsp.y0().var == 0x330000, "disabled command blocked another interrupt");
		queuedPort.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
		queued->advance();
		require(queued->handled(), "re-enabling a withdrawn command lost it");
		queued->dsp.regs().x.var = 0;
		queuedPort.writeControlRegister(0);
		queuedPort.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
		queued->advance();
		require(!queued->handled(), "enable toggling duplicated a serviced command");

		// An unrelated request may use the same vector without acknowledging HI08.
		auto shared = std::make_unique<Fixture>();
		auto& sharedPort = shared->peripherals.getHI08();
		sharedPort.setHostCommandArbitration(true);
		sharedPort.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
		sharedPort.writeControlRegister(0);
		sharedPort.writeHostCommand(0x20);
		shared->dsp.injectExternalInterrupt(0x20);
		shared->advance();
		require(shared->handled(), "unrelated same-vector interrupt did not execute");
		require(sharedPort.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP),
			"unrelated same-vector interrupt acknowledged HI08");

		// Reconfiguration invalidates a queued request even if a new command is
		// already pending by the time the DSP observes the old queue entry.
		auto reset = std::make_unique<Fixture>();
		auto& resetPort = reset->peripherals.getHI08();
		resetPort.setHostCommandArbitration(true);
		resetPort.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
		resetPort.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
		reset->dsp.regs().sr.var = 0x300;
		resetPort.writeHostCommand(0x20);
		reset->advance();
		require(reset->dsp.hasPendingInterrupts() && !reset->handled(),
			"reset fixture did not retain a masked CPU request");
		resetPort.setHostCommandArbitration(false);
		resetPort.setHostCommandArbitration(true);
		resetPort.writeHostCommand(0x22);
		reset->dsp.regs().sr.var = 0;
		reset->advance();
		require(!reset->handled(), "stale command executed after reconfiguration");
		require(reset->dsp.y0().var == 0x330000, "stale command displaced the new command");
		require(!(resetPort.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP)),
			"new command was not acknowledged after reconfiguration");
		// DSP56303UM table 6-13: hardware/software reset clears HCP.
		// Cover both a peripheral-only pending command and an IPL-masked CPU
		// request, including the serializer's extra queued command.
		for(const bool enabledBeforeReset : {false, true})
		{
			auto hardwareReset = std::make_unique<Fixture>();
			auto& resetPort = hardwareReset->peripherals.getHI08();
			resetPort.setHostCommandArbitration(true);
			resetPort.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			resetPort.writeControlRegister(enabledBeforeReset ? 1u << dsp56k::HDI08::HCR_HCIE : 0);
			hardwareReset->dsp.regs().sr.var = 0x300;
			resetPort.writeHostCommand(0x20);
			hardwareReset->advance();
			require(hardwareReset->dsp.hasPendingInterrupts() == enabledBeforeReset,
				"reset fixture did not reach the intended pending state");
			resetPort.writeHostCommand(0x22);
			resetPort.reset();
			require(!(resetPort.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP)),
				"hardware/software reset retained HCP");
			require(!resetPort.hostCommandBusy(), "reset retained command serializer state");
			resetPort.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			resetPort.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
			hardwareReset->dsp.regs().sr.var = 0;
			hardwareReset->advance();
			require(!hardwareReset->handled() && hardwareReset->dsp.y0().var == 0,
				"reset resurrected an old pending/queued command");
			resetPort.writeHostCommand(0x22);
			hardwareReset->advance();
			require(hardwareReset->dsp.y0().var == 0x330000,
				"fresh command did not execute after reset");
		}
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
		{
			mc68k::Hdi08 legacy;
			unsigned calls = 0;
			legacy.setWriteIrqCallback([&](uint8_t vector) { require(vector == 0x20, "legacy vector changed"); ++calls; });
			legacy.write8(mc68k::PeriphAddress::HdiCVR, mc68k::Hdi08::Hc | 0x10);
			require(calls == 1 && legacy.read8(mc68k::PeriphAddress::HdiCVR) == 0x10,
				"unconfigured host no longer acknowledges synchronously");
		}
		{
			auto accepted = std::make_unique<Fixture>();
			auto& port = accepted->peripherals.getHI08();
			port.setHostCommandArbitration(true);
			port.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			port.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
			port.writeHostCommand(0x20);
			for(unsigned i = 0; i < 100 && port.hostCommandPending(); ++i) accepted->dsp.exec();
			require(!port.hostCommandPending() && port.hostCommandBusy(),
				"fixture did not stop between command acceptance and return");
			port.writeHostCommand(0x22);
			require(port.hostCommandPending(), "serializer queue was acknowledged before acceptance");
			require(port.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP),
				"serializer queue did not retain HCP");
			port.cancelHostCommand();
			require(!port.hostCommandPending() && port.hostCommandBusy(),
				"cancelling pending delivery aborted the accepted handler");
			accepted->advance();
			require(accepted->handled() && accepted->dsp.y0().var == 0,
				"cancellation aborted an accepted handler or delivered the queued command");
		}
		std::cout << "HI08 hardware control: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "HI08 hardware control: " << error.what() << '\n';
		return 1;
	}
}
