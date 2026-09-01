#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdsim.h"
#include "dsp56kEmu/memory.h"

#include <cstdint>
#include <iostream>

namespace
{
	bool check(const bool _condition, const char* const _message)
	{
		if(_condition)
			return true;
		std::cerr << _message << '\n';
		return false;
	}

	bool testDspMemoryFallback()
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Memory memory(validator, 16, 16, 16);
		if(!check(!memory.hasMmuSupport(), "fallback memory unexpectedly uses an MMU mapping"))
			return false;

		memory.set(dsp56k::MemArea_Y, 3, 0x123456);
		return check(memory.get(dsp56k::MemArea_Y, 3) == 0x123456,
			"fallback memory did not preserve the written value")
			&& check(memory.get(dsp56k::MemArea_X, 3) == 0,
				"fallback memory did not preserve separate memory areas");
	}

	bool testMk2PortAInvertedLoopback()
	{
		md::Sim sim;
		sim.write8(md::Sim::g_ppddr, 0x04);
		sim.setMk2PortAInvertedLoopback(true);
		sim.write8(md::Sim::g_ppdat, 0x04);
		if(!check((sim.read8(md::Sim::g_ppdat) & 0x01) == 0,
			"MKII Port A loopback did not invert HIGH"))
			return false;
		sim.write8(md::Sim::g_ppdat, 0x00);
		return check((sim.read8(md::Sim::g_ppdat) & 0x01) != 0,
			"MKII Port A loopback did not invert LOW");
	}

	bool testFrontPanelStepLeds()
	{
		bool mmMappingMatches = true;
		for(uint32_t step = 0; step < 16; ++step)
		{
			md::FrontPanel panel;
			const uint8_t command = static_cast<uint8_t>(
				md::FrontPanel::g_firstLedBank + (step >> 2));
			const uint8_t bit = static_cast<uint8_t>(((step & 3) << 1) + 1);
			const uint8_t message[] = {command,
				static_cast<uint8_t>(~static_cast<uint8_t>(1u << bit))};
			panel.processBytes(message, sizeof(message));
			for(uint32_t candidate = 0; candidate < 16; ++candidate)
				mmMappingMatches &= panel.getMonomachineStepLed(candidate)
					== (candidate == step);
		}
		if(!check(mmMappingMatches, "Monomachine step LED mapping is wrong"))
			return false;

		md::FrontPanel panel;
		const uint8_t mdMessage[] = {0x20, 0xfe, 0x21, 0x7f};
		panel.processBytes(mdMessage, sizeof(mdMessage));
		return check(panel.getStepLed(0) && panel.getStepLed(15),
			"Machinedrum step LED mapping changed")
			&& check(!panel.getStepLed(1) && !panel.getStepLed(14),
				"Machinedrum unlit step decoding changed")
			&& check(!panel.getStepLed(16) && !panel.getMonomachineStepLed(16),
				"out-of-range step LED was accepted");
	}
}

int main()
{
	if(!testDspMemoryFallback() || !testMk2PortAInvertedLoopback()
		|| !testFrontPanelStepLeds())
		return 1;
	std::cout << "mdLib tests passed\n";
	return 0;
}
