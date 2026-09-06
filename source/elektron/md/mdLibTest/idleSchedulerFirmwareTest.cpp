#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"
#include "mc68k/cpuState.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace md
{
	struct HostRxFirmwareTestAccess
	{
		static void origin(Hardware& hw, unsigned index, uint64_t cycle)
		{
			auto& dsp = index ? hw.m_dspProducer : hw.m_dspMixer;
			hw.m_schedDspOriginLatched[index] = true;
			hw.m_schedDspOriginFrame[index] = static_cast<double>(cycle) / (40000000.0 / 44100);
			hw.m_schedDspOriginUcCycles[index] = cycle;
			hw.m_schedDspOriginCycles[index] = dsp.dsp().getCycles();
		}
		static void now(Hardware& hw, uint64_t cycle) { hw.m_schedUcCyclesDone = cycle; }
		static void target(Hardware& hw, double frame) { hw.m_schedFramesTotal = frame; }
		static bool step(Hardware& hw) { return hw.schedStep(); }
	};
}

namespace
{
	using Access = md::HostRxFirmwareTestAccess;
	using Sim = md::Sim;

	void require(bool condition, const char* message)
	{
		if(!condition)
			throw std::runtime_error(message);
	}

	std::unique_ptr<md::Hardware> machine(const std::vector<uint8_t>& rom,
		uint64_t now, unsigned source, uint64_t delta)
	{
		auto hw = std::make_unique<md::Hardware>(rom, "idle-boundary-test", md::MachineModel::Monomachine);
		require(hw->isValid(), "invalid hardware");
		auto& cpu = hw->getUC();
		cpu.write16(0x200080, 0x60fe);
		cpu.setPC(0x200080);
		m68k_set_reg(cpu.getCpuState(), M68K_REG_SR, 0x2000);
		m68k_set_reg(cpu.getCpuState(), M68K_REG_SP, 0x2fff00);
		m68k_set_reg(cpu.getCpuState(), M68K_REG_VBR, 0x200400);
		// Independently authored code: IRQ4 records the timer counter in D0,
		// then waits. Early or late delivery changes the compared register state.
		cpu.write16(0x200400 + 28 * 4, 0x0020);
		cpu.write16(0x200402 + 28 * 4, 0x0200);
		cpu.write16(0x200200, 0x3039);
		cpu.write16(0x200202, 0x0030);
		cpu.write16(0x200204, 0x010c);
		cpu.write16(0x200206, 0x60fe);
		auto& sim = cpu.getSim();
		sim.write16(Sim::g_imr, 0);
		sim.write8(Sim::g_icrExtIrq4, 4 << 2);
		sim.write16(Sim::g_timer1Base + Sim::g_timerTrr, 65535);
		sim.write16(Sim::g_timer1Base + Sim::g_timerTmr, Sim::g_tmrRst | 2);
		Access::now(*hw, now);
		for(unsigned index = 0; index < 2; ++index)
		{
			auto& dsp = index ? hw->getDspProducer() : hw->getDspMixer();
			auto& port = index ? cpu.getHdi08Dsp2() : cpu.getHdi08Dsp1();
			dsp.onDspBootFinished();
			Access::origin(*hw, index, now + (index == source ? delta : 10000));
			port.icr(mc68k::Hdi08::Rreq);
		}
		auto& producer = source ? hw->getDspProducer() : hw->getDspMixer();
		producer.hdi08().writeTX(0x123456);
		require(producer.hasDeferredHostRx() == (delta != 0), "staged deadline differs");
		return hw;
	}

	void compare(const std::vector<uint8_t>& rom, uint64_t now,
		unsigned source, uint64_t delta, uint64_t targetCycles)
	{
		auto a = machine(rom, now, source, delta);
		auto b = machine(rom, now, source, delta);
		const double target = static_cast<double>(now + targetCycles) / (40000000.0 / 44100);
		Access::target(*a, target);
		Access::target(*b, target);
		// Reference is the single-instruction CPU loop. Neither DSP runs: the
		// CPU starts behind both latched DSP origins and reaches this slice's end.
		do
		{
			a->processUC();
		}
		while(static_cast<double>(a->hostCurrentCycle()) / (40000000.0 / 44100) < target);
		require(Access::step(*b), "scheduler made no progress");
		require(a->hostCurrentCycle() == b->hostCurrentCycle(), "scheduler endpoint changed");
		require(a->getUC().getCycles() == b->getUC().getCycles(), "CPU cycle total changed");
		auto& portA = source ? a->getUC().getHdi08Dsp2() : a->getUC().getHdi08Dsp1();
		auto& portB = source ? b->getUC().getHdi08Dsp2() : b->getUC().getHdi08Dsp1();
		require(portA.hostRxWordsAvailable() == portB.hostRxWordsAvailable(),
			"publication moved across an instruction boundary");
		require((source ? a->getDspProducer() : a->getDspMixer()).hasDeferredHostRx()
			== (source ? b->getDspProducer() : b->getDspMixer()).hasDeferredHostRx(), "pending latch differs");
		if(targetCycles == 512)
			require(portB.hostRxWordsAvailable() == 1, "word never became visible");
		require(std::memcmp(static_cast<m68ki_cpu_core*>(a->getUC().getCpuState()),
			static_cast<m68ki_cpu_core*>(b->getUC().getCpuState()), sizeof(m68ki_cpu_core)) == 0,
			"core state or IRQ timer observation differs");
		require(a->getUC().getSim().read16(Sim::g_timer1Base + Sim::g_timerTcn)
			== b->getUC().getSim().read16(Sim::g_timer1Base + Sim::g_timerTcn), "timer progression differs");
	}

	void schedulerBoundaries(const std::vector<uint8_t>& rom)
	{
		unsigned cases = 0;
		for(uint64_t now : {uint64_t{0}, (uint64_t{1} << 40) + 8, uint64_t{40000000} * 86400})
			for(unsigned source = 0; source < 2; ++source)
				for(uint64_t delta : {uint64_t{0}, uint64_t{1}, uint64_t{2}, uint64_t{15},
					uint64_t{16}, uint64_t{17}, uint64_t{63}, uint64_t{129}})
					for(uint64_t target : {delta > 2 ? delta - 1 : uint64_t{2}, delta + 1, delta + 3, uint64_t{512}})
					{
						compare(rom, now, source, delta, target);
						++cases;
					}
		std::cout << "Scheduler: " << cases << " publication/core/timer comparisons passed\n";
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path || !*path)
	{
		std::cout << "Set GEARMULATOR_MM_FIRMWARE_BIN to run the idle scheduler integration test\n";
		return 77;
	}
	try
	{
		std::vector<uint8_t> rom;
		require(baseLib::filesystem::readFile(rom, path), "could not read firmware");
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Monomachine), "unsupported firmware");
		schedulerBoundaries(rom);
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
}
