#include "mdLib/mdmc.h"
#include "mdLib/mdrom.h"

#include <cstdio>
#include <vector>

namespace
{
	constexpr uint32_t g_flashBase = 0x10000000;
	constexpr uint32_t g_commandAa = g_flashBase + 0xaaaa;
	constexpr uint32_t g_command55 = g_flashBase + 0x5554;

	void program(md::Microcontroller& _uc, const uint32_t _address, const uint16_t _value)
	{
		_uc.write16(g_commandAa, 0xaaaa);
		_uc.write16(g_command55, 0x5555);
		_uc.write16(g_commandAa, 0xa0a0);
		_uc.write16(_address, _value);
	}

	void eraseSector(md::Microcontroller& _uc, const uint32_t _address)
	{
		_uc.write16(g_commandAa, 0xaaaa);
		_uc.write16(g_command55, 0x5555);
		_uc.write16(g_commandAa, 0x8080);
		_uc.write16(g_commandAa, 0xaaaa);
		_uc.write16(g_command55, 0x5555);
		_uc.write16(_address, 0x3030);
	}
}

int main()
{
	std::vector<uint8_t> bytes(md::g_romSize, 0xff);
	md::Rom rom(bytes, "synthetic-mm-flash.bin");
	md::Microcontroller mm(rom, md::MachineModel::Monomachine);

	constexpr uint32_t sector = g_flashBase + 0x200000;
	constexpr uint32_t target = sector + 0x1234;
	program(mm, target, 0x5aa5);
	if(mm.read16(target) != 0x5aa5)
	{
		std::puts("FAIL: Monomachine flash program command did not change the private image");
		return 1;
	}
	if(rom.data()[target - g_flashBase] != 0xff)
	{
		std::puts("FAIL: flash programming changed the source ROM image");
		return 1;
	}

	eraseSector(mm, sector);
	if(mm.read16(target) != 0xffff)
	{
		std::puts("FAIL: Monomachine 64 KiB sector erase did not complete");
		return 1;
	}

	auto savedUserFlash = mm.copyUserFlash();
	const auto savedOffset = target - (g_flashBase + 0x200000);
	savedUserFlash[savedOffset] = 0x12;
	savedUserFlash[savedOffset + 1] = 0x34;
	md::Microcontroller restored(rom, md::MachineModel::Monomachine, {}, {},
		savedUserFlash);
	if(restored.read16(target) != 0x1234)
	{
		std::puts("FAIL: saved Monomachine user flash was not restored");
		return 1;
	}

	md::Microcontroller md(rom, md::MachineModel::Machinedrum);
	program(md, target, 0x5aa5);
	if(md.read16(target) != 0x5aa5)
	{
		std::puts("FAIL: Machinedrum flash program command did not change the private image");
		return 1;
	}
	if(rom.data()[target - g_flashBase] != 0xff)
	{
		std::puts("FAIL: Machinedrum flash programming changed the source ROM image");
		return 1;
	}
	eraseSector(md, sector);
	if(md.read16(target) != 0xffff)
	{
		std::puts("FAIL: Machinedrum 64 KiB sector erase did not complete");
		return 1;
	}

	std::puts("PASS: MD/MM flash programs privately and MM storage restores");
	return 0;
}
