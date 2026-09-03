#include "baseLib/filesystem.h"
#include "mdLib/mdromdata.h"
#include "mdLib/mdstate.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int g_failures = 0;

	void check(const bool _condition, const char* const _message)
	{
		std::printf("[%s] %s\n", _condition ? "PASS" : "FAIL", _message);
		if(!_condition)
			++g_failures;
	}

	std::vector<uint8_t> makePatchRam()
	{
		std::vector<uint8_t> result(md::g_patchRamStateSize);
		uint32_t value = 0x12345678;
		for(auto& byte : result)
		{
			value = value * 1664525u + 1013904223u;
			byte = static_cast<uint8_t>(value >> 24);
		}
		return result;
	}

	uint32_t crc32(const uint8_t* const _data, const size_t _size)
	{
		uint32_t crc = 0xffffffffu;
		for(size_t i = 0; i < _size; ++i)
		{
			crc ^= _data[i];
			for(uint32_t bit = 0; bit < 8; ++bit)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
		return ~crc;
	}

	void writeU32(std::vector<uint8_t>& _data, const size_t _offset,
		const uint32_t _value)
	{
		_data[_offset] = static_cast<uint8_t>(_value >> 24);
		_data[_offset + 1] = static_cast<uint8_t>(_value >> 16);
		_data[_offset + 2] = static_cast<uint8_t>(_value >> 8);
		_data[_offset + 3] = static_cast<uint8_t>(_value);
	}

	void testSparseProjectState()
	{
		const auto patchRam = makePatchRam();
		std::vector<uint8_t> rom(md::g_romSize, 0xff);
		for(size_t i = 0; i < rom.size(); i += 4093)
			rom[i] = static_cast<uint8_t>(i >> 8);
		auto factory = rom;
		factory[md::g_uwFlashSectorSize + 7] = 0x61;

		std::vector<uint8_t> factoryOnly;
		check(md::encodeStateWithFactoryBaseline(factoryOnly, patchRam, factory,
			factory, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal),
			"validated factory-relative UW state encodes");
		check(factoryOnly.size() == 52 + patchRam.size(),
			"ROM-equal or initialized factory data stays out of project state");

		// Equality with the ROM is not evidence that no validated factory cache
		// exists. This is the compactness regression fixed after PR #8.
		std::vector<uint8_t> romEqualFactory;
		check(md::encodeStateWithFactoryBaseline(romEqualFactory, patchRam, rom,
			rom, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal),
			"ROM-equal validated factory baseline encodes");
		check(romEqualFactory.size() == 52 + patchRam.size(),
			"ROM-equal validated baseline remains exactly header plus patch RAM");

		auto flashA = factory;
		flashA[3 * md::g_uwFlashSectorSize + 19] = 0x18;
		flashA.back() = 0x00;
		std::vector<uint8_t> encodedA;
		check(md::encodeStateWithFactoryBaseline(encodedA, patchRam, flashA,
			factory, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal), "two-sector UW state encodes");
		constexpr size_t changedSectors = 2;
		constexpr size_t stateHeaderSize = 52;
		constexpr size_t entryHeaderSize = 8;
		check(encodedA.size() == stateHeaderSize + patchRam.size()
			+ changedSectors * (entryHeaderSize + md::g_uwFlashSectorSize),
			"UW state contains exactly the two changed sectors");

		md::DecodedState decodedA;
		std::vector<uint8_t> restoredA;
		check(md::decodeState(decodedA, encodedA, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state decodes against its ROM");
		check(decodedA.containsFlash && decodedA.patchRam == patchRam,
			"UW decode preserves patch RAM and flash marker");
		check(md::applyFlashOverlay(restoredA, decodedA.flashOverlay, factory),
			"UW sparse overlay applies to its factory baseline");
		check(restoredA == flashA, "patch RAM and flash round-trip byte exactly");

		// Version 3 was briefly published before its header-integrity gap was
		// discovered. Keep those Monday-night project states readable while all new
		// states use version 4 and bind the baseline metadata into the flash CRC.
		auto version3State = encodedA;
		version3State[4] = 0;
		version3State[5] = 3;
		const auto overlayOffset = stateHeaderSize + patchRam.size();
		writeU32(version3State, 48, crc32(version3State.data() + overlayOffset,
			version3State.size() - overlayOffset));
		md::DecodedState decodedVersion3;
		check(md::decodeState(decodedVersion3, version3State, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"briefly published version-3 UW state remains compatible");

		auto flashB = factory;
		flashB[7 * md::g_uwFlashSectorSize + 11] = 0x77;
		std::vector<uint8_t> encodedB;
		md::DecodedState decodedB;
		std::vector<uint8_t> restoredB;
		check(md::encodeStateWithFactoryBaseline(encodedB, patchRam, flashB,
			factory, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal)
			&& md::decodeState(decodedB, encodedB, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
			&& md::applyFlashOverlay(restoredB, decodedB.flashOverlay, factory),
			"second UW instance state round-trips");
		check(restoredB == flashB && restoredB != restoredA,
			"two UW instances retain isolated flash images");

		auto wrongRom = rom;
		wrongRom[123] ^= 1;
		md::DecodedState unchanged;
		unchanged.patchRam = {1, 2, 3};
		check(!md::decodeState(unchanged, encodedA, wrongRom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state rejects the wrong ROM");
		check(unchanged.patchRam == std::vector<uint8_t>({1, 2, 3}),
			"wrong-ROM rejection leaves decoded output unchanged");
		auto wrongFactory = factory;
		wrongFactory[456] ^= 1;
		const std::vector<uint8_t> applySentinel{4, 5, 6};
		std::vector<uint8_t> unchangedFlash = applySentinel;
		check(!md::applyFlashOverlay(unchangedFlash, decodedA.flashOverlay,
			wrongFactory), "UW state rejects the wrong factory baseline");
		check(unchangedFlash == applySentinel,
			"wrong-factory rejection leaves flash output unchanged");

		auto corrupt = encodedA;
		corrupt.back() ^= 1;
		check(!md::decodeState(unchanged, corrupt, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state rejects corrupt flash CRC data");
		auto corruptBaselineFingerprint = encodedA;
		corruptBaselineFingerprint[28] ^= 1;
		check(!md::decodeState(unchanged, corruptBaselineFingerprint, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state rejects corrupt baseline metadata");
		auto truncated = encodedA;
		truncated.pop_back();
		check(!md::decodeState(unchanged, truncated, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state rejects truncation");
		auto trailing = encodedA;
		trailing.push_back(0);
		check(!md::decodeState(unchanged, trailing, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state rejects trailing bytes");

		// Before a machine-local factory cache exists, every sector must be carried.
		// This records even a factory-populated sector erased back to ROM bytes.
		auto firstRunFlash = factory;
		std::copy_n(rom.begin() + md::g_uwFlashSectorSize,
			md::g_uwFlashSectorSize,
			firstRunFlash.begin() + md::g_uwFlashSectorSize);
		firstRunFlash[9 * md::g_uwFlashSectorSize + 31] = 0x27;
		std::vector<uint8_t> firstRunState;
		md::DecodedState decodedFirstRun;
		std::vector<uint8_t> restoredFirstRun;
		check(md::encodeState(firstRunState, patchRam, firstRunFlash, rom, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"pre-baseline UW fallback state encodes");
		check(firstRunState.size() == stateHeaderSize + patchRam.size()
			+ (md::g_romSize / md::g_uwFlashSectorSize)
				* (entryHeaderSize + md::g_uwFlashSectorSize),
			"pre-baseline fallback contains every flash sector");
		check(md::decodeState(decodedFirstRun, firstRunState, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
			&& md::applyFlashOverlay(restoredFirstRun,
				decodedFirstRun.flashOverlay, factory),
			"complete fallback decodes without the original baseline");
		check(restoredFirstRun == firstRunFlash,
			"complete fallback preserves a ROM-equal deletion");

		std::vector<uint8_t> legacy;
		md::DecodedState decodedLegacy;
		check(md::encodeState(legacy, patchRam, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal)
			&& md::decodeState(decodedLegacy, legacy, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"legacy version-1 Machinedrum state remains compatible");
		check(!decodedLegacy.containsFlash && decodedLegacy.patchRam == patchRam,
			"legacy state changes patch RAM without replacing flash");
	}

	void testFactoryCache()
	{
		std::vector<uint8_t> rom(md::g_romSize, 0xff);
		rom.front() = 0x12;
		rom.back() = 0x34;
		auto initialized = rom;
		initialized[2 * md::g_uwFlashSectorSize + 9] = 0x56;

		std::vector<uint8_t> cache;
		std::vector<uint8_t> decoded;
		check(md::encodeFactoryFlashCache(cache, initialized, rom),
			"UW factory cache encodes");
		check(cache.size() == 36 + 8 + md::g_uwFlashSectorSize,
			"factory cache contains exactly one sparse sector");
		check(cache.size() < md::g_romSize,
			"factory cache does not copy the complete ROM");
		check(md::decodeFactoryFlashCache(decoded, cache, rom),
			"UW factory cache decodes");
		check(decoded == initialized, "UW factory cache round-trips byte exactly");

		auto wrongRom = rom;
		wrongRom[1234] ^= 1;
		decoded = {1, 2, 3};
		check(!md::decodeFactoryFlashCache(decoded, cache, wrongRom),
			"factory cache rejects the wrong ROM");
		check(decoded == std::vector<uint8_t>({1, 2, 3}),
			"wrong-ROM cache rejection leaves output unchanged");
		auto corrupt = cache;
		corrupt.back() ^= 1;
		check(!md::decodeFactoryFlashCache(decoded, corrupt, rom),
			"factory cache rejects corrupted sector data");
	}

	void testCacheFilePublication()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto filename = baseLib::filesystem::getCurrentDirectory()
			+ ".md-cache-exclusive-test-" + std::to_string(nonce);
		const std::vector<uint8_t> first{1, 2, 3, 4};
		const std::vector<uint8_t> second{9, 8, 7};
		const std::vector<uint8_t> recovered{5, 6, 7, 8, 9};
		std::vector<uint8_t> readback;
		check(baseLib::filesystem::writeFileExclusive(filename, first),
			"immutable factory cache is created exclusively");
		check(!baseLib::filesystem::writeFileExclusive(filename, second),
			"second exclusive writer cannot replace the cache");
		check(baseLib::filesystem::writeFileAtomic(filename, recovered),
			"invalid cache can be replaced atomically");
		check(baseLib::filesystem::readFile(readback, filename)
			&& readback == recovered,
			"atomic replacement publishes the complete new cache");
		baseLib::filesystem::remove(filename);
	}
}

int main()
{
	std::puts("Machinedrum UW state and cache tests\n");
	testSparseProjectState();
	testFactoryCache();
	testCacheFilePublication();
	std::printf("\n%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILED",
		g_failures);
	return g_failures == 0 ? 0 : 1;
}
