#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t g_canonicalRomSize = 0x800000;
	constexpr uint64_t g_canonicalMd163Fnv1a = 0x33b7c1a9e29f43fdull;
	constexpr uint64_t g_canonicalMm132bFnv1a = 0xe1c1b461b6d0f21bull;

	bool check(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return true;
		std::cerr << "FAIL: " << _message << '\n';
		return false;
	}

	bool testStrictSupportedImages()
	{
		using md::MachineModel;
		if(!check(md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMd163Fnv1a, MachineModel::Machinedrum),
			"Machinedrum 1.63 fingerprint was rejected")
			|| !check(md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMm132bFnv1a, MachineModel::Monomachine),
				"Monomachine 1.32b fingerprint was rejected")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize - 1u,
				g_canonicalMm132bFnv1a, MachineModel::Monomachine),
				"incomplete Monomachine image was accepted")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize + 1u,
				g_canonicalMd163Fnv1a, MachineModel::Machinedrum),
				"oversized Machinedrum image was accepted")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMm132bFnv1a, MachineModel::Machinedrum),
				"Monomachine image was accepted for Machinedrum")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMd163Fnv1a, MachineModel::Monomachine),
				"Machinedrum image was accepted for Monomachine"))
			return false;

		std::vector<uint8_t> unsupported(g_canonicalRomSize, 0xff);
		return check(!md::RomLoader::isRomForModel(
				unsupported, MachineModel::Machinedrum),
			"unsupported complete Machinedrum image was accepted")
			&& check(!md::RomLoader::isRomForModel(
				unsupported, MachineModel::Monomachine),
			"unsupported complete Monomachine image was accepted");
	}
}

int main()
{
	if(!testStrictSupportedImages())
		return 1;
	std::cout << "MD/MM firmware image validation: PASS\n";
	return 0;
}
