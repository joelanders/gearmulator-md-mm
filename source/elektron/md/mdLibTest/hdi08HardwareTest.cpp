#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "mc68k/hdi08.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
	struct Fixture
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Peripherals56303 peripherals;
		dsp56k::PeripheralsNop unused;
		dsp56k::Memory memory{validator, 0x10000, 0x10000, 0x8000};
		dsp56k::DSP dsp{memory, &peripherals, &unused};
		explicit Fixture(bool longHandler = false)
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
			if(longHandler)
			{
				auto config = dsp.getJit().getConfig();
				config.maxInstructionsPerBlock = 4;
				dsp.getJit().setConfig(config);
				// exec() may run eight JIT blocks. Keep the handler alive across
				// that batch so acceptance can be inspected before RTI.
				for(unsigned i = 0; i < 128; ++i) handler = emit(handler, "nop");
			}
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

	void reportInterruptTiming()
	{
		for(const unsigned variant : {0u, 1u, 2u})
		{
			const bool longHandler = variant == 1;
			auto fixture = std::make_unique<Fixture>(longHandler);
			auto& dsp = fixture->dsp;
			if(variant == 2)
			{
				dsp56k::Assembler assembler;
				const auto marker = assembler.assemble("move #$5a,x0");
				require(marker.success() && marker.wordCount == 1, "fast marker assembly failed");
				dsp.memWriteP(0x20, marker.word[0]);
				dsp.memWriteP(0x21, 0); // NOP: no JSR, hence a two-word fast handler.
			}
			auto config = dsp.getJit().getConfig();
			config.maxInstructionsPerBlock = 1;
			config.linkJitBlocks = false;
			config.dynamicFastInterrupts = true;
			dsp.getJit().setConfig(config);
			unsigned accepted = 0;
			bool unexpectedVector = false;
			uint64_t acceptedCycle = 0;
			dsp.setInterruptServicedCallback([&](dsp56k::TWord vector)
			{
				unexpectedVector |= vector != 0x20;
				++accepted;
				acceptedCycle = dsp.getCycles();
			});
			auto& port = fixture->peripherals.getHI08();
			port.setHostCommandArbitration(true);
			port.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			port.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
			const auto start = dsp.getCycles();
			port.writeHostCommand(0x20);
			for(unsigned step = 0; step < 4096 && !fixture->handled(); ++step)
				dsp.execUntilCycles(dsp.getCycles() + 1);
			require(fixture->handled() && accepted == 1 && !unexpectedVector,
				"timing probe did not handle exactly one expected command");
			const auto marked = dsp.getCycles();
			for(unsigned step = 0; step < 4096 && dsp.getPC().var != 0x100; ++step)
				dsp.execUntilCycles(dsp.getCycles() + 1);
			require(dsp.getPC().var == 0x100, "timing probe did not return to synthetic main loop");
			std::cout << "Interrupt timing " << (dsp56k::g_useJIT ? "jit" : "interpreter")
				<< " fast " << (variant == 2)
				<< " handler-nops " << (longHandler ? 128 : 0)
				<< " accepted-cycle " << acceptedCycle - start
				<< " marker-cycle " << marked - start
				<< " returned-cycle " << dsp.getCycles() - start << '\n';
		}
	}
}

int main(int argc, char** argv)
{
	try
	{
		if(argc == 2 && std::string_view(argv[1]) == "--timing")
		{
			reportInterruptTiming();
			return 0;
		}
		if(argc != 1) return 2;
		{
			mc68k::Hdi08 host;
			host.setRxEmptyCallback([](bool) {});
			bool delivered = false;
			host.setReadIsrCallback([&](uint8_t sampled)
			{
				if(!delivered)
				{
					require(!(sampled & mc68k::Hdi08::Rxdf), "receive fixture was not initially empty");
					delivered = true; // writeRx may re-enter status observation.
					host.writeRx(0x123456);
					require(!host.canReceiveData(), "callback did not populate the receive latch");
				}
				return host.refreshReceiveStatus(sampled);
			});
			require(host.read8(mc68k::PeriphAddress::HdiISR) & mc68k::Hdi08::Rxdf,
				"status returned stale RXDF after callback latched data");
			require(host.read8(mc68k::PeriphAddress::HdiTXH) == 0x12
				&& host.read8(mc68k::PeriphAddress::HdiTXM) == 0x34
				&& host.read8(mc68k::PeriphAddress::HdiTXL) == 0x56,
				"receive-status refresh changed the latched word");
			const auto flags = mc68k::Hdi08::Rxdf | mc68k::Hdi08::Hf2 | mc68k::Hdi08::Txde;
			require(host.refreshReceiveStatus(flags) == (flags & ~mc68k::Hdi08::Rxdf),
				"receive-status refresh retained consumed RXDF or changed unrelated flags");
		}
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
			auto accepted = std::make_unique<Fixture>(true);
			auto& port = accepted->peripherals.getHI08();
			port.setHostCommandArbitration(true);
			port.writePortControlRegister(1u << dsp56k::HDI08::HPCR_HEN);
			port.writeControlRegister(1u << dsp56k::HDI08::HCR_HCIE);
			port.writeHostCommand(0x20);
			for(unsigned i = 0; i < 100 && port.hostCommandPending(); ++i) accepted->dsp.exec();
			require(!port.hostCommandPending()
				&& accepted->dsp.getProcessingMode() == dsp56k::DSP::LongInterrupt,
				"fixture did not stop between command acceptance and return");
			port.writeHostCommand(0x22);
			require(port.hostCommandPending(), "second command was acknowledged before acceptance");
			port.exec();
			require(accepted->dsp.hasQueuedInterrupts(), "second command waited for the first handler to return");
			require(port.readStatusRegister() & (1u << dsp56k::HDI08::HSR_HCP),
				"second command did not retain HCP");
			port.cancelHostCommand();
			require(!port.hostCommandPending()
				&& accepted->dsp.getProcessingMode() == dsp56k::DSP::LongInterrupt,
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
