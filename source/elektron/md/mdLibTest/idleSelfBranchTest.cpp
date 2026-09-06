#include "mdLib/mdmc.h"
#include "mdLib/mdrom.h"
#include "mc68k/cpuState.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace md
{
	struct IdleSelfBranchTestAccess
	{
		static uint32_t limit(Microcontroller& c, uint32_t cycles)
		{
			return c.idleSelfBranchInstructions(cycles);
		}
		static void skip(Microcontroller& c, uint32_t instructions)
		{
			c.advanceIdleSelfBranch(instructions);
		}
		static void panel(Microcontroller& c, uint32_t divider)
		{
			c.m_panelDisplayReady = true;
			c.m_panelDisplayReadyDivider = divider;
		}
		static uint32_t divider(const Microcontroller& c)
		{
			return c.m_panelDisplayReadyDivider;
		}
	};
}

namespace
{
	using Access = md::IdleSelfBranchTestAccess;
	using Sim = md::Sim;

	void require(bool c, const char* message)
	{
		if(!c)
			throw std::runtime_error(message);
	}

	const md::Rom rom(std::vector<uint8_t>(md::g_romSize, 0), "generated-empty-test-image");

	std::unique_ptr<md::Microcontroller> cpu(uint32_t pc = 0x200080)
	{
		auto c = std::make_unique<md::Microcontroller>(rom, md::MachineModel::Monomachine);
		c->write16(pc, 0x60fe);
		c->setPC(pc);
		m68k_set_reg(c->getCpuState(), M68K_REG_SR, 0x2000);
		m68k_set_reg(c->getCpuState(), M68K_REG_SP, 0x2fff00);
		m68k_set_reg(c->getCpuState(), M68K_REG_VBR, 0x200400);
		// Independently authored interrupt handlers branch to themselves. Each vector
		// has a distinct handler so a wrong level/order changes the compared PC.
		for(unsigned vector = 24; vector < 32; ++vector)
		{
			const uint32_t address = 0x200100 + vector * 4;
			c->write16(0x200400 + vector * 4, address >> 16);
			c->write16(0x200402 + vector * 4, address);
			c->write16(address, 0x60fe);
		}
		require(c->exec() == 2, "initial branch cycle count");
		return c;
	}

	void equal(md::Microcontroller& a, md::Microcontroller& b)
	{
		require(a.getCycles() == b.getCycles(), "CPU cycles differ");
		require(std::memcmp(static_cast<m68ki_cpu_core*>(a.getCpuState()),
			static_cast<m68ki_cpu_core*>(b.getCpuState()), sizeof(m68ki_cpu_core)) == 0,
			"CPU architectural/internal state differs");
		require(Access::divider(a) == Access::divider(b), "panel instruction divider differs");
		for(unsigned base : {Sim::g_timer1Base, Sim::g_timer2Base})
			for(unsigned offset : {Sim::g_timerTmr, Sim::g_timerTrr, Sim::g_timerTcn, Sim::g_timerTer})
				require(a.getSim().read16(base + offset) == b.getSim().read16(base + offset),
					"timer register differs");
		require(a.getSim().cyclesUntilNextTimerInterrupt() == b.getSim().cyclesUntilNextTimerInterrupt(), "timer deadline differs");
		require(a.getSim().needsInterruptCheck() == b.getSim().needsInterruptCheck(), "SIM interrupt scan state differs");
		for(uint32_t p = 0x2ffe80; p < 0x2fff00; p += 2)
			require(a.read16(p) == b.read16(p), "interrupt stack frame differs");
		for(unsigned level = 0; level < 8; ++level)
			for(unsigned vector = 24; vector < 32; ++vector)
				require(a.hasPendingInterrupt(vector, level) == b.hasPendingInterrupt(vector, level),
					"pending interrupt differs");
	}

	uint64_t skips = 0;

	void run(md::Microcontroller& c, uint32_t cycles, bool accelerated)
	{
		const auto stop = c.getCycles() + cycles;
		while(c.getCycles() < stop)
		{
			auto limit = accelerated ? Access::limit(c, static_cast<uint32_t>(stop - c.getCycles())) : 0;
			if(limit)
			{
				Access::skip(c, limit);
				skips += limit;
			}
			else
				c.exec();
		}
	}

	void timerConfiguration(bool restart, bool masked, unsigned prescale,
		unsigned clock, unsigned reference)
	{
		auto a = cpu();
		auto b = cpu();
		for(auto* c : {a.get(), b.get()})
		{
			auto& s = c->getSim();
			s.write16(Sim::g_imr, masked?0x3ffe:0);
			for(unsigned index = 0; index < 2; ++index)
			{
				auto base = index?Sim::g_timer2Base:Sim::g_timer1Base;
				s.write8(index?Sim::g_icrTimer2:Sim::g_icrTimer1, (index+1)<<2);
				s.write16(base+Sim::g_timerTrr, static_cast<uint16_t>(reference));
				s.write16(base + Sim::g_timerTmr, static_cast<uint16_t>(((prescale - 1) << 8)
					| (clock << 1) | Sim::g_tmrRst | Sim::g_tmrOri | (restart ? Sim::g_tmrFrr : 0)));
			}
		}
		// Compare at odd/even boundaries, across reference and counter wrap, then
		// clear a latched event and unmask/reconfigure it between scheduler slices.
		for(unsigned count : {1u, 2u, 15u, 16u, 17u, 61u, 1024u, 65539u})
		{
			run(*a, count, false);
			run(*b, count, true);
			equal(*a, *b);
		}
		for(auto* c : {a.get(), b.get()})
		{
			c->getSim().write8(Sim::g_timer1Base+Sim::g_timerTer, Sim::g_terRef);
			c->getSim().write16(Sim::g_imr, 0);
		}
		run(*a, 257, false);
		run(*b, 257, true);
		equal(*a, *b);
	}

	void timerMatrix()
	{
		unsigned cases = 0;
		for(bool restart : {false, true})
			for(bool masked : {false, true})
				for(unsigned prescale : {1u, 2u, 17u, 256u})
					for(unsigned clock : {1u, 2u})
						for(unsigned reference : {0u, 1u, 7u, 63u, 65535u})
						{
							timerConfiguration(restart, masked, prescale, clock, reference);
							++cases;
						}
		std::cout << "Timer matrix: " << cases << " configurations passed\n";
	}

	void guards()
	{
		for(uint32_t pc : {0x200080u, 0x2a0040u, 0x01000020u})
		{
			auto a = cpu(pc);
			auto b = cpu(pc);
			require(Access::limit(*b, 33) == 16, "odd cycle bound rounded up");
			require(Access::limit(*b, 15) == 0, "small batch accepted");
			run(*a, 1000, false);
			run(*b, 1000, true);
			equal(*a, *b);
			b->write16(pc, 0x4e71);
			require(Access::limit(*b, 1000) == 0, "modified instruction was skipped");
			b->write16(pc, 0x60fc);
			require(Access::limit(*b, 1000) == 0, "non-self branch accepted");
			b->write16(pc, 0x60fe);
			auto& state = *b->getCpuState();
			for(auto field : {&state.nmi_pending, &state.stopped, &state.reset_cycles,
				&state.t0_flag, &state.t1_flag, &state.run_mode})
			{
				auto old = *field;
				*field = 1;
				require(Access::limit(*b, 1000) == 0, "CPU guard failed");
				*field = old;
			}
			state.pmmu_enabled = 1;
			require(Access::limit(*b, 1000) == 0, "MMU mode accepted");
			state.pmmu_enabled = 0;
			auto oldPpc = state.ppc;
			state.ppc += 2;
			require(Access::limit(*b, 1000) == 0, "nonfixed program counter accepted");
			state.ppc = oldPpc;
			state.m68ki_initial_cycles = 2;
			require(Access::limit(*b, 1000) == 0, "non-single-instruction accounting accepted");
			state.m68ki_initial_cycles = 1;
			auto type = state.cpu_type;
			state.cpu_type = CPU_TYPE_020;
			require(Access::limit(*b, 1000) == 0, "legacy CPU accepted");
			state.cpu_type = type;
			b->injectInterrupt(28, 4);
			require(Access::limit(*b, 1000) == 0, "serviceable interrupt was skipped");
		}
		auto a = cpu();
		auto b = cpu();
		for(uint32_t divider : {0u, 0x3ff0u, 0x3ffeu, 0x3fffu, 0xfffffff0u})
		{
			Access::panel(*a, divider);
			Access::panel(*b, divider);
			run(*a, 100, false);
			run(*b, 100, true);
			equal(*a, *b);
		}
		auto c = cpu();
		c->getSim().write16(Sim::g_imr, 0);
		c->getSim().write8(Sim::g_icrExtIrq4, 4 << 2);
		require(Access::limit(*c, 1000) == 0, "dirty SIM configuration was skipped");
		c->exec();
		c->getSim().setExternalIrq4(true);
		require(Access::limit(*c, 1000) == 0, "asserted external IRQ was skipped");
		m68k_set_reg(c->getCpuState(), M68K_REG_SR, 0x2700);
		c->exec();
		c->getSim().setExternalIrq4(false);
		require(Access::limit(*c, 1000) == 0, "pending external IRQ removal was skipped");
		c->exec();
		require(!c->hasPendingInterrupt(28, 4), "external IRQ not removed");
		require(Access::limit(*c, 1000) > 0, "idle acceleration did not resume");
		std::cout<<"CPU, code mutation, panel divider and external IRQ guards passed\n";
	}
}

int main()
{
	try
	{
		guards();
		timerMatrix();
		require(skips > 100000, "accelerated path was not exercised");
		std::cout<<"Compared "<<skips<<" skipped instructions with individual execution\n";
	}
	catch(const std::exception& e)
	{
		std::cerr<<e.what()<<'\n';
		return 1;
	}
}
