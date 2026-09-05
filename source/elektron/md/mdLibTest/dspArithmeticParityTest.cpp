// Firmware-free differential diagnostic. Neither backend is an ISA oracle.
#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

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

	int reportCacheLoop()
	{
		if(!dsp56k::g_useJIT) return 77;
		bool failed = false;
		for(const bool clearCache : {false, true})
		{
			auto fixture = std::make_unique<Fixture>();
			auto& dsp = fixture->dsp;
			auto config = dsp.getJit().getConfig();
			config.maxDoIterations = 1;
			dsp.getJit().setConfig(config);
			dsp56k::Assembler assembler;
			unsigned pc = 0x100;
			for(const auto* instruction : {"do #$5,>$104", "add b,a", "nop", "nop"})
			{
				const auto code = assembler.assemble(instruction);
				if(!code.success()) throw std::runtime_error("cache-loop assembly failed");
				for(unsigned i = 0; i < code.wordCount; ++i) dsp.memWriteP(pc++, code.word[i]);
			}
			dsp.regs().sr.var = 0;
			dsp.regs().lc.var = 0x321;
			dsp.regs().a.var = 0;
			// Accumulators use the same left-aligned storage as the parity fixture.
			dsp.regs().b.var = uint64_t(0x1000000) << 8;
			const auto accumulator = [&]() { return dsp.regs().a.var >> 8; };
			dsp.setPC(0x100);
			for(unsigned steps = 0; steps < 10 && dsp.getPC().var != 0x103; ++steps)
				dsp.execUntilCycles(dsp.getCycles() + 1);
			if(dsp.getPC().var != 0x103 || dsp.regs().lc.var != 5
				|| accumulator() != 0x1000000)
				throw std::runtime_error("cache-loop did not pause after first addition");
			if(clearCache) dsp.getJit().destroyAllBlocks();
			for(unsigned steps = 0; steps < 100 && dsp.getPC().var != 0x104; ++steps)
				dsp.execUntilCycles(dsp.getCycles() + 1);
			const bool ok = dsp.getPC().var == 0x104 && accumulator() == 0x5000000
				&& dsp.regs().lc.var == 0x321 && !dsp.sr_test_noCache(dsp56k::SR_LF);
			std::cout << "Cache-loop clear " << clearCache << " result " << ok
				<< " A " << accumulator() << " LC " << dsp.regs().lc.var
				<< " PC " << dsp.getPC().var << '\n';
			failed |= !ok;
		}
		return failed ? 1 : 0;
	}

	int reportCycles()
	{
		dsp56k::Assembler assembler;
		for(const auto* body : {"nop", "add x0,a"})
		for(const unsigned repeats : {1u, 4u, 16u})
		{
			auto fixture = std::make_unique<Fixture>();
			auto& dsp = fixture->dsp;
			uint32_t end = 0x100;
			const auto emit = [&](const std::string& instruction)
			{
				const auto code = assembler.assemble(instruction.c_str());
				if(!code.success()) throw std::runtime_error("cycle probe assembly failed");
				for(unsigned i = 0; i < code.wordCount; ++i)
					dsp.memWriteP(end++, code.word[i]);
			};
			if(repeats > 1) emit("rep #" + std::to_string(repeats));
			emit(body);
			dsp.regs().sr.var = 0;
			dsp.regs().lc.var = 0x321;
			dsp.regs().x.var = 1;
			dsp.setPC(0x100);
			const auto cycles = dsp.getCycles();
			const auto instructions = dsp.getInstructionCounter();
			// Execute one dispatch unit through this build's normal backend.
			// REP is indivisible here; this is not a peripheral-deadline oracle.
			dsp.execUntilCycles(cycles + 1);
			if(dsp.getPC().var != end || dsp.regs().lc.var != 0x321)
				throw std::runtime_error("cycle probe did not finish exactly one unit");
			std::cout << "Cycles " << (dsp56k::g_useJIT ? "jit" : "interpreter")
				<< " body " << body << " repeats " << repeats
				<< " cycles " << dsp.getCycles() - cycles
				<< " instructions " << dsp.getInstructionCounter() - instructions << '\n';
		}
		return 0;
	}
}

int main(int argc, char** argv)
{
	if(argc == 2 && std::string(argv[1]) == "--cache-loop")
	{
		try { return reportCacheLoop(); }
		catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
	}
	if(argc == 2 && std::string(argv[1]) == "--cycles")
	{
		try { return reportCycles(); }
		catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
	}
	if(argc != 1) return 2;
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
		for(const unsigned repeats : {1u, 4u, 16u})
		{
			bool matched = true;
			seed = 0x56303; // Same inputs per instruction, independent of prior failures.
			const auto code = assembler.assemble(instruction);
			if(!code.success()) throw std::runtime_error(instruction);
			const auto prefixText = "rep #" + std::to_string(repeats);
			const auto prefix = assembler.assemble(prefixText.c_str());
			if(!prefix.success() || code.wordCount != 1)
				throw std::runtime_error("repeat diagnostic requires a one-word body");
			for(auto* fixture : {interpreter.get(), jit.get()})
			{
				if(repeats > 1)
					fixture->dsp.memWriteP(pc, prefix.word[0]);
				for(unsigned word = 0; word < code.wordCount; ++word)
					fixture->dsp.memWriteP(pc + (repeats > 1 ? 1 : 0) + word, code.word[word]);
			}
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
					dsp.regs().lc.var = 0x321;
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
					|| i.regs().lc.var != j.regs().lc.var
					|| i.getSR().var != j.getSR().var || i.getPC().var != j.getPC().var)
				{
					std::cerr << "Mismatch " << instruction << " repeats " << std::dec << repeats
						<< " trial " << trial << std::hex
						<< " input a=" << a << " b=" << b << " x=" << x << " y=" << y << " sr=" << sr
						<< " interpreter a=" << i.regs().a.var << " sr=" << i.getSR().var << " pc=" << i.getPC().var
						<< " lc=" << i.regs().lc.var
						<< " jit a=" << j.regs().a.var << " sr=" << j.getSR().var << " pc=" << j.getPC().var
						<< " lc=" << j.regs().lc.var << '\n';
					++failures;
					matched = false;
					break;
				}
			}
			if(matched) std::cout << "Parity " << instruction << " repeats " << std::dec << repeats << '\n';
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
