#include "mdLib/mdstate.h"

#include <cstdio>
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

	std::vector<uint8_t> makeUserFlash()
	{
		std::vector<uint8_t> result(md::g_mmUserFlashStateSize);
		for(size_t i = 0; i < result.size(); ++i)
			result[i] = static_cast<uint8_t>((i * 29u + (i >> 9)) & 0xffu);
		return result;
	}
}

int main()
{
	std::printf("md state codec test\n\n");

	const auto original = makePatchRam();
	std::vector<uint8_t> encoded = {1, synthLib::StateTypeGlobal};
	check(md::encodeState(encoded, original, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal), "global Monomachine state encodes after plugin envelope");
	check(encoded.size() == 2 + 20 + original.size(), "encoded state has exact expected size");

	const std::vector<uint8_t> payload(encoded.begin() + 2, encoded.end());
	std::vector<uint8_t> decoded = {0xaa};
	check(md::decodeState(decoded, payload, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal), "matching state decodes");
	check(decoded == original, "patch RAM round-trips byte exactly");

	const auto originalUserFlash = makeUserFlash();
	std::vector<uint8_t> encodedWithFlash;
	check(md::encodeState(encodedWithFlash, original, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal, originalUserFlash),
		"Monomachine state encodes mutable DigiPRO flash storage");
	check(encodedWithFlash.size() == 28 + original.size() + originalUserFlash.size(),
		"DigiPRO state has the exact version-2 size");
	std::vector<uint8_t> decodedUserFlash;
	check(md::decodeState(decoded, decodedUserFlash, encodedWithFlash,
		md::MachineModel::Monomachine, synthLib::StateTypeGlobal),
		"version-2 Monomachine state decodes");
	check(decoded == original && decodedUserFlash == originalUserFlash,
		"patch RAM and DigiPRO flash round-trip byte exactly");
	decodedUserFlash = {0x55};
	check(md::decodeState(decoded, decodedUserFlash, payload,
		md::MachineModel::Monomachine, synthLib::StateTypeGlobal)
		&& decodedUserFlash.empty(),
		"legacy version-1 Monomachine state remains compatible");

	std::vector<uint8_t> currentProgram;
	check(md::encodeState(currentProgram, original, md::MachineModel::Machinedrum,
		synthLib::StateTypeCurrentProgram), "current-program Machinedrum state encodes");
	check(md::decodeState(decoded, currentProgram, md::MachineModel::Machinedrum,
		synthLib::StateTypeCurrentProgram), "current-program Machinedrum state decodes");

	decoded = {0x55};
	check(!md::decodeState(decoded, payload, md::MachineModel::Machinedrum,
		synthLib::StateTypeGlobal), "wrong machine model is rejected");
	check(decoded == std::vector<uint8_t>{0x55}, "failed decode leaves output untouched");
	check(!md::decodeState(decoded, payload, md::MachineModel::Monomachine,
		synthLib::StateTypeCurrentProgram), "wrong state type is rejected");

	auto corrupt = payload;
	corrupt.back() ^= 0x80;
	check(!md::decodeState(decoded, corrupt, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal), "payload corruption is rejected by CRC");

	auto truncated = payload;
	truncated.pop_back();
	check(!md::decodeState(decoded, truncated, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal), "truncated payload is rejected");

	auto trailing = payload;
	trailing.push_back(0);
	check(!md::decodeState(decoded, trailing, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal), "trailing bytes are rejected");

	std::vector<uint8_t> invalidRam(original.size() - 1);
	std::vector<uint8_t> unchanged = {0x12, 0x34};
	check(!md::encodeState(unchanged, invalidRam, md::MachineModel::Monomachine,
		synthLib::StateTypeGlobal), "wrong patch-RAM size is rejected");
	check(unchanged == std::vector<uint8_t>({0x12, 0x34}),
		"failed encode leaves destination untouched");

	std::printf("\n%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
	return g_failures == 0 ? 0 : 1;
}
