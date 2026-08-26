#include "mdromloader.h"

namespace md
{
	Rom RomLoader::findROM()
	{
		return findROM(MachineModel::Machinedrum);
	}

	Rom RomLoader::findROM(const MachineModel _model)
	{
		const auto files = findFiles(".bin", g_romSize, g_romSize);

		if(files.empty())
			return {};

		for (const auto& file : files)
		{
			auto rom = Rom(file);
			if(rom.isValid() && isRomForModel(rom.data(), _model))
				return rom;
		}
		return {};
	}

	bool RomLoader::isRomForModel(const std::vector<uint8_t>& _data,
		const MachineModel _model)
	{
		if(_data.size() != g_romSize)
			return false;

		uint64_t fingerprint = 14695981039346656037ull;
		for(const auto byte : _data)
		{
			fingerprint ^= byte;
			fingerprint *= 1099511628211ull;
		}

		// Accept only a supported image for the requested product.
		return _model == MachineModel::Monomachine
			? fingerprint == g_mmOs132bFingerprint
			: fingerprint == g_mdOs163Fingerprint;
	}
}
