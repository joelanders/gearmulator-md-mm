#include "hardwareLib/am29f.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
	struct CommandAddresses
	{
		uint32_t aa;
		uint32_t fiveFive;
	};

	constexpr CommandAddresses g_straight{0x555, 0x2aa};
	constexpr CommandAddresses g_reversed{0xaaaa, 0x5554};

	void program(hwLib::Am29f& _flash, const CommandAddresses& _commands,
		const uint32_t _address, const uint16_t _value)
	{
		_flash.write(_commands.aa, 0xaaaa);
		_flash.write(_commands.fiveFive, 0x5555);
		_flash.write(_commands.aa, 0xa0a0);
		_flash.write(_address, _value);
	}

	void eraseSector(hwLib::Am29f& _flash, const CommandAddresses& _commands,
		const uint32_t _address)
	{
		_flash.write(_commands.aa, 0xaaaa);
		_flash.write(_commands.fiveFive, 0x5555);
		_flash.write(_commands.aa, 0x8080);
		_flash.write(_commands.aa, 0xaaaa);
		_flash.write(_commands.fiveFive, 0x5555);
		_flash.write(_address, 0x3030);
	}

	void eraseChip(hwLib::Am29f& _flash, const CommandAddresses& _commands)
	{
		_flash.write(_commands.aa, 0xaaaa);
		_flash.write(_commands.fiveFive, 0x5555);
		_flash.write(_commands.aa, 0x8080);
		_flash.write(_commands.aa, 0xaaaa);
		_flash.write(_commands.fiveFive, 0x5555);
		_flash.write(_commands.aa, 0x1010);
	}

	uint16_t readWord(const std::vector<uint8_t>& _bytes, const uint32_t _address)
	{
		return static_cast<uint16_t>((static_cast<uint16_t>(_bytes[_address]) << 8)
			| _bytes[_address + 1]);
	}

	bool check(const bool _condition, const char* const _message)
	{
		if(_condition)
			return true;
		std::fprintf(stderr, "FAIL: %s\n", _message);
		return false;
	}
}

int main()
{
	constexpr size_t flashSize = 512 * 1024;
	std::vector<uint8_t> bytes(flashSize, 0xff);
	hwLib::Am29f straight(bytes.data(), bytes.size(), false, false);
	program(straight, g_straight, 0x1234, 0x5aa5);
	if(!check(readWord(bytes, 0x1234) == 0x5aa5,
		"straight-address program command failed"))
		return 1;

	std::fill(bytes.begin(), bytes.end(), 0xff);
	hwLib::Am29f reversed(bytes.data(), bytes.size(), false, true);
	constexpr uint32_t collisionTarget = 0x20aaa;
	program(reversed, g_reversed, collisionTarget, 0x12aa);
	if(!check(readWord(bytes, collisionTarget) == 0x12aa,
		"program data matching an erase unlock cycle was not written"))
		return 1;
	reversed.write(g_reversed.aa, 0xaaaa);
	if(!check(readWord(bytes, g_reversed.aa) == 0xffff,
		"the next unlock cycle was programmed as stale command data"))
		return 1;
	reversed.write(g_reversed.fiveFive, 0x5555);
	reversed.write(g_reversed.aa, 0xa0a0);
	reversed.write(0x1234, 0x34c7);
	if(!check(readWord(bytes, 0x1234) == 0x34c7,
		"the command following collision data did not execute normally"))
		return 1;

	std::fill(bytes.begin(), bytes.end(), 0x00);
	eraseSector(reversed, g_reversed, 0x20000);
	if(!check(std::all_of(bytes.begin() + 0x20000, bytes.begin() + 0x30000,
		[](const uint8_t _byte) { return _byte == 0xff; }),
		"sector erase did not erase its 64 KiB sector")
		|| !check(bytes[0x1ffff] == 0x00 && bytes[0x30000] == 0x00,
			"sector erase changed bytes outside its sector"))
		return 1;

	std::fill(bytes.begin(), bytes.end(), 0x00);
	eraseChip(reversed, g_reversed);
	if(!check(std::all_of(bytes.begin(), bytes.end(),
		[](const uint8_t _byte) { return _byte == 0xff; }),
		"chip erase did not execute on its final command cycle"))
		return 1;

	bytes.back() = 0x5a;
	program(reversed, g_reversed, static_cast<uint32_t>(bytes.size() - 1), 0x0000);
	if(!check(bytes.back() == 0x5a, "out-of-range word program changed the buffer tail")
		|| !check(!reversed.eraseSector(static_cast<uint32_t>(bytes.size() - 1024), 2),
			"out-of-range sector erase was accepted")
		|| !check(bytes.back() == 0x5a, "out-of-range sector erase changed the buffer tail"))
		return 1;

	std::puts("PASS: AMD flash command sequencing and bounds");
	return 0;
}
