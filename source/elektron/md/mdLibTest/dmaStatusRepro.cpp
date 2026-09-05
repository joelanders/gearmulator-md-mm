// Public DSP56300FM table 10-10: DTD clears after the DE-enable pipeline delay.
// No firmware, active serial ports, DMA requests, or interrupts are supplied.
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"

#include <iostream>
#include <memory>

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
}

int main()
{
	bool failed = false;
	for(unsigned channel = 0; channel < 6; ++channel)
	{
		auto fixture = std::make_unique<Fixture>();
		auto& dsp = fixture->dsp;
		auto& dma = fixture->peripherals.getDMA();
		auto config = dsp.getJit().getConfig();
		config.maxInstructionsPerBlock = 1;
		config.linkJitBlocks = false;
		dsp.getJit().setConfig(config);
		for(unsigned pc = 0x100; pc < 0x140; ++pc) dsp.memWriteP(pc, 0); // NOP
		dsp.setPC(0x100);
		dsp.regs().sr.var = 0x300;
		const auto reset = dma.getDSTR() & 0x3f;
		dma.setDSR(channel, 0x1000);
		dma.setDDR(channel, 0x2000);
		dma.setDCO(channel, 3);
		// External IRQA, word/request/clear-DE mode; no external edge is raised.
		dma.setDCR(channel, (44u << dsp56k::DmaChannel::Dam0)
			| (1u << dsp56k::DmaChannel::Dtm0) | (1u << dsp56k::DmaChannel::De));
		const auto start = dsp.getCycles();
		// Deliberately beyond the documented three-instruction pipeline delay.
		dsp.execUntilCycles(start + 16);
		const auto pending = dma.getDSTR() & 0x3f;
		const auto expected = 0x3fu & ~(1u << channel);
		const bool enabled = (dma.getDCR(channel) & (1u << dsp56k::DmaChannel::De)) != 0;
		const bool untouched = dma.getDSR(channel) == 0x1000 && dma.getDDR(channel) == 0x2000
			&& dma.getDCO(channel) == 3;
		std::cout << "DMA status channel " << channel << " reset " << reset
			<< " enabled " << enabled << " untouched " << untouched << " after-cycles "
			<< dsp.getCycles() - start << " done-mask " << pending << " expected " << expected << '\n';
		failed |= reset != 0x3f || !enabled || !untouched || pending != expected;
	}
	if(failed) std::cerr << "DMA transfer-done status does not reflect enabled waiting channels\n";
	return failed ? 1 : 0;
}
