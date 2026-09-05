// Firmware-free differential diagnostic. Neither backend is an ISA oracle.
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
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
		dsp56k::PeripheralsNop x, y;
		dsp56k::Memory memory{validator, 0x10000, 0x10000, 0x8000};
		dsp56k::DSP dsp{memory, &x, &y};
		Fixture()
		{
			auto config = dsp.getJit().getConfig();
			config.maxInstructionsPerBlock = 1;
			config.linkJitBlocks = false;
			dsp.getJit().setConfig(config);
		}
	};
}

int main()
{
	if constexpr(!dsp56k::g_useJIT)
	{
		std::cout << "SKIP: parity diagnostic requires a JIT-enabled build\n";
		return 77;
	}
	try
	{
		auto interpreter = std::make_unique<Fixture>();
		auto jit = std::make_unique<Fixture>();
		dsp56k::Assembler assembler;
		uint64_t seed = 0x56303;
		const auto next = [&]()
		{
			seed ^= seed << 13;
			seed ^= seed >> 7;
			seed ^= seed << 17;
			return seed;
		};
		uint32_t pc = 0x100;
		unsigned failures = 0;
		for(const auto* instruction : {"clr a", "abs a", "neg a", "add x0,a", "add b,a",
			"sub x0,a", "sub b,a", "mac x0,y0,a", "mpy x0,y0,a", "mpyr y0,x0,a",
			"macr y0,x0,a", "rnd a", "asr a", "asl a", "addr b,a", "addl b,a", "move a,x0"})
		{
			bool matched = true;
			const auto code = assembler.assemble(instruction);
			if(!code.success()) throw std::runtime_error(instruction);
			for(auto* fixture : {interpreter.get(), jit.get()})
				for(unsigned word = 0; word < code.wordCount; ++word)
					fixture->dsp.memWriteP(pc + word, code.word[word]);
			for(unsigned trial = 0; trial < 1024; ++trial)
			{
				const auto a = next() & 0xffffffffffffffull;
				const auto b = next() & 0xffffffffffffffull;
				const auto x = next() & 0xffffffffffffull;
				const auto y = next() & 0xffffffffffffull;
				// Exercise normal, scale-down, and scale-up modes, preserving input CCR.
				const auto sr = uint32_t(next() & 0xff) | ((trial % 3) << 10);
				for(auto* fixture : {interpreter.get(), jit.get()})
				{
					auto& dsp = fixture->dsp;
					(void)dsp.getSR(); // Resolve lazy flags before replacing the input state.
					dsp.regs().sr.var = sr;
					// The core stores 56-bit accumulators left-aligned in 64 bits.
					dsp.regs().a.var = a << 8;
					dsp.regs().b.var = b << 8;
					dsp.regs().x.var = x;
					dsp.regs().y.var = y;
					dsp.setPC(pc);
				}
				interpreter->dsp.execInterpreter();
				// Direct synthetic SR setup must select the matching specialized JIT chain.
				jit->dsp.getJit().checkModeChange();
				jit->dsp.execJit();
				auto& i = interpreter->dsp;
				auto& j = jit->dsp;
				if(i.regs().a.var != j.regs().a.var || i.regs().b.var != j.regs().b.var
					|| i.regs().x.var != j.regs().x.var || i.regs().y.var != j.regs().y.var
					|| i.getSR().var != j.getSR().var || i.getPC().var != j.getPC().var)
				{
					std::cerr << "Mismatch " << instruction << " trial " << trial << std::hex
						<< " input a=" << a << " b=" << b << " x=" << x << " y=" << y << " sr=" << sr
						<< " interpreter a=" << i.regs().a.var << " sr=" << i.getSR().var << " pc=" << i.getPC().var
						<< " jit a=" << j.regs().a.var << " sr=" << j.getSR().var << " pc=" << j.getPC().var << '\n';
					++failures;
					matched = false;
					break;
				}
			}
			if(matched) std::cout << "Parity " << instruction << '\n';
			pc += 4;
		}
		return failures ? 1 : 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 2;
	}
}
