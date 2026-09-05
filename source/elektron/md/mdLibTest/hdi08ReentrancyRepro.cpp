// Manual reproducer for the unresolved receive-latch overwrite.
// Expected current result: exit 1, first byte 0x44 instead of 0x11.
#include "mc68k/hdi08.h"
#include <iostream>

int main()
{
	mc68k::Hdi08 host;
	host.setRxEmptyCallback([](bool) {});
	bool injectSecond = true;
	host.setReadIsrCallback([&](uint8_t status)
	{
		if(injectSecond)
		{
			injectSecond = false;
			host.writeRx(0x445566);
		}
		return status;
	});
	host.writeRx(0x112233);
	const auto first = host.read8(mc68k::PeriphAddress::HdiTXH);
	std::cout << "First receive byte: " << std::hex << unsigned(first) << '\n';
	if(first != 0x11)
	{
		std::cerr << "Nested status observation replaced the first receive word\n";
		return 1;
	}
	return 0;
}
