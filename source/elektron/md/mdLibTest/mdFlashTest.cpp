#include "mdLib/mdmc.h"
#include "mdLib/mdrom.h"

#include <cstdio>
#include <vector>

namespace
{
	constexpr uint32_t g_flashBase = 0x10000000;
	constexpr uint32_t g_commandAa = g_flashBase + 0xaaaa;
	constexpr uint32_t g_command55 = g_flashBase + 0x5554;

	void program(md::Microcontroller& _uc, const uint32_t _address,
		const uint16_t _value)
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

	int fail(const char* const _message)
	{
		std::puts(_message);
		return 1;
	}
}

int main()
{
	std::vector<uint8_t> bytes(md::g_romSize, 0xff);
	md::Rom rom(bytes, "synthetic-md-flash.bin");
	md::Microcontroller flash(rom, md::MachineModel::Machinedrum);

	constexpr uint32_t regularSector = g_flashBase + 0x200000;
	constexpr uint32_t target = regularSector + 0x1234;
	program(flash, target, 0x5aa5);
	if(flash.read16(target) != 0x5aa5)
		return fail("FAIL: program command did not change the private flash image");
	if(rom.data()[target - g_flashBase] != 0xff)
		return fail("FAIL: programming changed the source ROM image");

	// Payload at an unlock-shaped address/value must still be treated as payload.
	constexpr uint32_t collisionTarget = regularSector + 0x0aaa;
	program(flash, collisionTarget, 0x12aa);
	if(flash.read16(collisionTarget) != 0x12aa)
		return fail("FAIL: program payload was confused with an unlock cycle");

	// A partial unlock must not arm a stale program operation.
	flash.write16(g_commandAa, 0xaaaa);
	if(flash.read16(g_commandAa) != 0xffff)
		return fail("FAIL: partial unlock programmed a stale word");
	flash.write16(g_command55, 0x5555);
	flash.write16(g_commandAa, 0xa0a0);
	flash.write16(target, 0x5aa5);

	eraseSector(flash, regularSector);
	if(flash.read16(target) != 0xffff)
		return fail("FAIL: regular 64 KiB sector erase did not complete");

	constexpr uint32_t firstBootBlock = g_flashBase;
	constexpr uint32_t secondBootBlock = g_flashBase + 0x2000;
	constexpr uint32_t thirdBootBlock = g_flashBase + 0x4000;
	constexpr uint32_t finalBootBlock = g_flashBase + 0xe000;
	constexpr uint32_t firstRegularBlock = g_flashBase + 0x10000;
	constexpr uint32_t secondRegularBlock = g_flashBase + 0x20000;
	program(flash, firstBootBlock + 0x12, 0x1111);
	program(flash, secondBootBlock + 0x12, 0x1234);
	program(flash, thirdBootBlock + 0x12, 0x5678);
	program(flash, finalBootBlock + 0x12, 0x9abc);
	program(flash, firstRegularBlock + 0x12, 0xdef0);
	program(flash, secondRegularBlock + 0x12, 0x1357);

	eraseSector(flash, firstBootBlock);
	if(flash.read16(firstBootBlock + 0x12) != 0xffff
		|| flash.read16(secondBootBlock + 0x12) != 0x1234)
		return fail("FAIL: first bottom-boot erase crossed an 8 KiB block");
	eraseSector(flash, secondBootBlock);
	if(flash.read16(secondBootBlock + 0x12) != 0xffff
		|| flash.read16(thirdBootBlock + 0x12) != 0x5678)
		return fail("FAIL: bottom-boot erase crossed an 8 KiB block");
	eraseSector(flash, finalBootBlock);
	if(flash.read16(finalBootBlock + 0x12) != 0xffff
		|| flash.read16(firstRegularBlock + 0x12) != 0xdef0)
		return fail("FAIL: final bottom-boot erase crossed the 64 KiB boundary");
	eraseSector(flash, firstRegularBlock);
	if(flash.read16(firstRegularBlock + 0x12) != 0xffff
		|| flash.read16(secondRegularBlock + 0x12) != 0x1357)
		return fail("FAIL: regular erase crossed a 64 KiB block");

	std::puts("PASS: Machinedrum flash command sequencing and erase geometry");
	return 0;
}
