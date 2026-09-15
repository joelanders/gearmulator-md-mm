#include "mdLib/mdmc.h"
#include "mdLib/mdrom.h"
#include "hardwareLib/am29f.h"
#include "mc68k/memoryOps.h"

#include <algorithm>
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

	// The factory bootloader (both models) erases/programs the OS region
	// through the absolute 0x00100000 window with word writes, then polls the
	// sector until it reads back erased. MIDI UPGRADE wedged on exactly this
	// poll because the window resolved to patch RAM instead of flash.
	constexpr uint32_t g_blUnlockAa = 0xaaaa;
	constexpr uint32_t g_blUnlock55 = 0x5554;
	constexpr uint32_t g_blSector = 0x120000;

	void blEraseSector(md::Microcontroller& _uc, const uint32_t _address, const bool _resetFirst)
	{
		if(_resetFirst)
			_uc.write16(g_blUnlockAa, 0xf0f0);
		_uc.write16(g_blUnlockAa, 0xaaaa);
		_uc.write16(g_blUnlock55, 0x5555);
		_uc.write16(g_blUnlockAa, 0x8080);
		_uc.write16(g_blUnlockAa, 0xaaaa);
		_uc.write16(g_blUnlock55, 0x5555);
		_uc.write16(_address, 0x3030);
	}

	void blProgram(md::Microcontroller& _uc, const uint32_t _address, const uint16_t _value)
	{
		_uc.write16(g_blUnlockAa, 0xaaaa);
		_uc.write16(g_blUnlock55, 0x5555);
		_uc.write16(g_blUnlockAa, 0xa0a0);
		_uc.write16(_address, _value);
	}

	bool pollErased(md::Microcontroller& _uc, const uint32_t _address)
	{
		for(int i = 0; i < 1000; ++i)
		{
			if((_uc.read16(_address) & 0xffff) == 0xffff)
				return true;
		}
		return false;
	}

	bool pollWord(md::Microcontroller& _uc, const uint32_t _address, const uint16_t _value)
	{
		for(int i = 0; i < 1000; ++i)
		{
			if((_uc.read16(_address) & 0xffff) == _value)
				return true;
		}
		return false;
	}

	int fail(const char* const _message)
	{
		std::puts(_message);
		return 1;
	}

	int bootloaderWindowMain();
}

int main()
{
	// MM uses the shared AMD decoder, not MD's separate command decoder.
	// TRI--INV contains 08AA at an unlock-shaped address and used to stall its
	// firmware flash programmer. Every possible programmed word is payload.
	for(bool reversed : {false, true})
	{
		std::vector<uint8_t> backing(0x10000, 0xff);
		hwLib::Am29f shared(backing.data(), backing.size(), false, reversed);
		const uint32_t aa = reversed ? 0xaaa : 0x555;
		const uint32_t bb = reversed ? 0x554 : 0x2aa;
		const uint32_t collision = reversed ? 0x2aaa : 0x2000;
		for(uint32_t word = 0; word <= 0xffff; ++word)
		{
			backing[collision] = backing[collision + 1] = 0xff;
			shared.write(aa, 0xaaaa);
			shared.write(bb, 0x5555);
			shared.write(aa, 0xa0a0);
			shared.write(collision, uint16_t(word));
			if(mc68k::memoryOps::readU16(backing, collision) != word)
			{
				std::printf("Shared flash mismatch: reversed=%u word=%04x actual=%04x\n", reversed, word,
					mc68k::memoryOps::readU16(backing, collision));
				return fail("FAIL: shared AMD flash mistook program data for a command");
			}
		}
		shared.write(aa, 0xaaaa);
		shared.write(bb, 0x5555);
		shared.write(aa, 0xa0a0);
		shared.write(collision, 0);
		shared.write(aa, 0xaaaa);
		shared.write(bb, 0x5555);
		shared.write(aa, 0x8080);
		shared.write(aa, 0xaaaa);
		shared.write(bb, 0x5555);
		shared.write(0, 0x3030);
		if(!std::all_of(backing.begin(), backing.end(), [](uint8_t byte) { return byte == 0xff; }))
			return fail("FAIL: shared AMD flash sector erase regressed");
	}
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

	std::puts("PASS: MD/shared AMD flash programming, command sequencing, and erase geometry");

	if(const int windowRc = bootloaderWindowMain())
		return windowRc;
	return 0;
}

namespace
{
	int bootloaderWindowMain()
	{
		// Both models, fresh 8 MiB erased image, no patch RAM: the window
		// reads flash, the bootloader erase poll converges, programming
		// sticks, and the alias observes the same cells.
		for(const auto model : {md::MachineModel::Machinedrum, md::MachineModel::Monomachine})
		{
			const bool isMd = model == md::MachineModel::Machinedrum;
			std::vector<uint8_t> bytes(md::g_romSize, 0xff);
			md::Rom rom(bytes, "synthetic-bl-flash.bin");
			md::Microcontroller uc(rom, model);

			// Unrelated flashLow traffic first, then the fresh window must
			// still read RAM zeros (sectors start as RAM; only an executed
			// flash command flips its sector to flash).
			uc.write16(g_commandAa, 0xaaaa);
			if(uc.read16(g_blSector + 0x10) != 0x0000)
				return fail("FAIL: fresh window did not read RAM");

			// Dirty the sector through the RAM side, then the bootloader
			// erase must execute against flash and win the poll back.
			uc.write16(g_blSector + 0x10, 0x1234);
			blEraseSector(uc, g_blSector, isMd);
			if(!pollErased(uc, g_blSector + 0x10))
				return fail("FAIL: bootloader sector erase poll never converged");
			if(uc.read16(g_blSector + 0x10) != 0xffff)
				return fail("FAIL: bootloader sector erase did not clear flash");
			// The 0x00700000 alias observes the same erased cells.
			if(uc.read16(0x700000 + (g_blSector - 0x100000) + 0x10) != 0xffff)
				return fail("FAIL: patch alias diverged from the window");

		// Bootloader program + poll, as UPDATING FLASH does it.
		blProgram(uc, g_blSector + 0x20, 0x5a5a);
		if(!pollWord(uc, g_blSector + 0x20, 0x5a5a))
		{
			std::printf("program poll readback=%04x model=%s\n",
				uc.read16(g_blSector + 0x20), isMd ? "MD" : "MM");
			return fail("FAIL: bootloader program poll never converged");
		}

		// The OS decompress copy: plain writes accumulate RAM and flip
			// the window to it, including read-back of fresh bytes.
			uc.write16(0x100000, 0xbeef);
			if(uc.read16(0x100000) != 0xbeef)
				return fail("FAIL: decompressed copy not visible after plain write");
			uc.write16(0x100002, 0x1234);
			if(uc.read16(0x100002) != 0x1234)
				return fail("FAIL: decompressed copy read-back mismatch");

		// The bootloader's plain variable writes (boot flag) must not defeat
		// a later sector erase: per-sector marks, last-writer-wins. Dirty
		// flash first so the erase has something observable to clear.
		blProgram(uc, 0x100040, 0x5a5a);
		if(!pollWord(uc, 0x100040, 0x5a5a))
			return fail("FAIL: setup program poll never converged");
		uc.write8(0x100409, 0x02);
		if(uc.read8(0x100409) != 0x02)
			return fail("FAIL: bootloader variable write not visible");
		blEraseSector(uc, 0x100000, isMd);
		if(!pollErased(uc, 0x100040))
			return fail("FAIL: erase after flag write never converged");

		// A later flash command (next upgrade) re-selects flash.
		blEraseSector(uc, g_blSector, isMd);
		if(!pollErased(uc, g_blSector + 0x20))
			return fail("FAIL: re-erase after RAM phase never converged");

		// The upgrader also erases low 8 KiB boot blocks (0x4000-0xFFFF,
		// sparing the reset vectors) plus low 64 KiB sectors, all through
		// absolute low addresses.
		for(uint32_t sector = 0x4000; sector < 0x10000; sector += 0x2000)
		{
			blProgram(uc, sector + 0x10, 0x5a5a);
			blEraseSector(uc, sector, isMd);
			if(!pollErased(uc, sector + 0x10))
				return fail("FAIL: 8 KiB boot block erase never converged");
		}
		blEraseSector(uc, 0x10000, isMd);
		if(!pollErased(uc, 0x10010))
			return fail("FAIL: low 64 KiB sector erase never converged");
		}

		// Untouched sectors read RAM zeros, a restored image runs from RAM
		// with no marker check, and a flash command still flips its sector
		// to flash afterwards.
		{
			std::vector<uint8_t> bytes(md::g_romSize, 0xaa);
			md::Rom rom(bytes, "synthetic-bl-flash2.bin");
			std::vector<uint8_t> ram(0x100000, 0);
			ram[0x00] = 0xbe; ram[0x01] = 0xef;
			md::Microcontroller fresh(rom, md::MachineModel::Monomachine, {});
			if(fresh.read16(0x100000) != 0x0000)
				return fail("FAIL: untouched sector did not read RAM zeros");
			md::Microcontroller restored(rom, md::MachineModel::Monomachine, ram);
			if(restored.read16(0x100000) != 0xbeef)
				return fail("FAIL: restored image not visible in RAM window");
			blEraseSector(restored, 0x100000, false);
			blProgram(restored, 0x100000, 0x5a5a);
			if(!pollWord(restored, 0x100000, 0x5a5a))
				return fail("FAIL: program after restore never converged");
		}

		std::puts("PASS: bootloader 0x100000-window flash/RAM dual backing");
		return 0;
	}
}
