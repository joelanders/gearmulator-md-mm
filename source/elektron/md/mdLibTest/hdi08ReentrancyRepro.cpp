// Manual reproducer for the unresolved receive-latch overwrite.
// The retained baseline exits 1: 0x445566 replaces 0x112233.
#include "mc68k/hdi08.h"
#include <iostream>

int main()
{
	for(const bool littleEndian : {false, true})
	{
		mc68k::Hdi08 host;
		host.icr(littleEndian ? mc68k::Hdi08::Hlend : 0);
		host.setRxEmptyCallback([&](bool needMore)
		{
			if(!needMore) host.relatchRx();
		});
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
		for(const uint32_t expected : {0x112233u, 0x445566u})
		{
			const auto first = host.read8(littleEndian ? mc68k::PeriphAddress::HdiTXL : mc68k::PeriphAddress::HdiTXH);
			const auto middle = host.read8(mc68k::PeriphAddress::HdiTXM);
			const auto last = host.read8(littleEndian ? mc68k::PeriphAddress::HdiTXH : mc68k::PeriphAddress::HdiTXL);
			const uint32_t word = (uint32_t(first) << 16) | (uint32_t(middle) << 8) | last;
			std::cout << (littleEndian ? "LE" : "BE") << " receive word: " << std::hex << word << '\n';
			if(word != expected)
			{
				std::cerr << "Nested status observation lost or reordered receive data\n";
				return 1;
			}
		}
		if(host.hostRxWordsAvailable() != 0 || !host.canReceiveData())
		{
			std::cerr << "Consumed receive words remain pending\n";
			return 1;
		}
	}
	return 0;
}
