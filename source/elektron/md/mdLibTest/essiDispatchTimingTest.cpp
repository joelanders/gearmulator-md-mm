#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <algorithm>
#include <array>
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
	};

	void measure(unsigned blockSize, bool repeat, unsigned slotCycles)
	{
		auto fixture = std::make_unique<Fixture>();
		auto& dsp = fixture->dsp;
		auto config = dsp.getJit().getConfig();
		config.maxInstructionsPerBlock = blockSize;
		config.linkJitBlocks = false;
		dsp.getJit().setConfig(config);
		dsp56k::Assembler assembler;
		unsigned pc = 0x100;
		const auto emit = [&](const char* instruction)
		{
			const auto code = assembler.assemble(instruction);
			if(!code.success()) throw std::runtime_error("synthetic assembly failed");
			for(unsigned i = 0; i < code.wordCount; ++i) dsp.memWriteP(pc++, code.word[i]);
		};
		if(repeat)
		{
			emit("rep #128");
			emit("nop");
		}
		else
			for(unsigned i = 0; i < 128; ++i) emit("nop");
		emit("jmp $100");
		dsp.regs().sr.var = 0x300; // Mask interrupts: this measures peripheral service only.
		dsp.setPC(0x100);

		auto& clock = fixture->peripherals.getEssiClock();
		auto& essi = fixture->peripherals.getEssi0();
		clock.setClockSource(dsp56k::EsxiClock::ClockSource::Cycles);
		clock.setCyclesPerSample(1152);
		clock.setExactCycleDeadlineEnabled(true);
		essi.setFineLinkMode(true);
		// Synthetic 8-bit/PM=0 or 24-bit/PM=1, PSR=1 configurations:
		// 16 or 96 cycles/slot, one slot/frame. No firmware-derived values.
		essi.writeCRA((1u << dsp56k::Essi::CRA_PSR)
			| (slotCycles == 96 ? (3u << dsp56k::Essi::CRA_WL0) | 1u : 0u));
		essi.writeCRB((1u << dsp56k::Essi::CRB_TE0) | (1u << dsp56k::Essi::CRB_SCKD));
		if(!clock.usesExactCycleDeadline()) throw std::runtime_error("fine deadline was not enabled");
		const auto start = dsp.getCycles();
		std::array<uint64_t, 128> observed{};
		unsigned count = 0;
		bool overflow = false;
		essi.setWriteTxCallback([&](uint64_t& frame, const dsp56k::Audio::TxFrame&)
		{
			if(count < observed.size()) observed[count++] = dsp.getCycles() - start;
			else overflow = true;
			++frame;
		});
		// Normal execution in a separately compiled JIT or interpreter build.
		while(dsp.getCycles() - start < 512) dsp.execUntilCycles(dsp.getCycles() + 1);
		if(overflow || count == 0) throw std::runtime_error("invalid serial observation count");
		uint64_t maxLate = 0;
		unsigned batched = 0;
		for(unsigned i = 0; i < count; ++i)
		{
			const uint64_t nominal = uint64_t(slotCycles) * (i + 1);
			if(observed[i] < nominal) throw std::runtime_error("serial service preceded its deadline");
			maxLate = std::max(maxLate, observed[i] - nominal);
			if(i && observed[i] == observed[i - 1]) ++batched;
		}
		std::cout << (dsp56k::g_useJIT ? "jit" : "interpreter") << " slot-period " << slotCycles
			<< " block-cap " << blockSize << " stop-cycle " << dsp.getCycles() - start
			<< " rep128 " << repeat << " slots " << count << " max-service-lateness " << maxLate
			<< " same-cycle-slots " << batched << " first-slots";
		for(unsigned i = 0; i < std::min(count, 10u); ++i) std::cout << ' ' << observed[i];
		std::cout << '\n';
	}
}

int main()
{
	try
	{
		for(const auto slotCycles : {16u, 96u})
			for(const auto blockSize : {1u, 32u})
				for(const bool repeat : {false, true}) measure(blockSize, repeat, slotCycles);
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
