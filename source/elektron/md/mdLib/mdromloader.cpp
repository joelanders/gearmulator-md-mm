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

	bool RomLoader::isSupportedImage(const size_t _size,
		const uint64_t _fingerprint, const MachineModel _model)
	{
		if(_size != g_romSize)
			return false;
		return _model == MachineModel::Monomachine
			? _fingerprint == g_mmOs132bFingerprint
			: _fingerprint == g_mdOs163Fingerprint;
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
		return isSupportedImage(_data.size(), fingerprint, _model);
	}
}
