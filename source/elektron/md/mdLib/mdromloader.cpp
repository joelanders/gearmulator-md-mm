#include "mdromloader.h"

#include "mdfirmwareupdate.h"

#include "baseLib/filesystem.h"

#include <cstdio>

namespace md
{
	Rom RomLoader::findROM()
	{
		return findROM(MachineModel::Machinedrum);
	}

	Rom RomLoader::findROM(const MachineModel _model)
	{
		const auto files = findFiles(".bin", g_romSize, g_romSize);
		for (const auto& file : files)
		{
			auto rom = Rom(file);
			if(rom.isValid() && isRomForModel(rom.data(), _model))
				return rom;
		}

		// Official MD/MM updates can be placed in the same roms directory as a
		// traditional dump. A real 8 MiB .bin remains the first choice; otherwise
		// convert the .syx to Gearmulator's private update image in memory.
		for(const auto& file : findFiles(".syx", 1, 16u * 1024u * 1024u))
		{
			std::vector<uint8_t> sysex;
			if(!baseLib::filesystem::readFile(sysex, file))
				continue;
			std::vector<uint8_t> converted;
			MachineModel model = MachineModel::Machinedrum;
			std::string error;
			if(!firmwareUpdate::convertSysexToRom(sysex, converted, model, error))
			{
				std::fprintf(stderr, "[MD/MM] ignoring OS update %s: %s\n",
					file.c_str(), error.c_str());
				continue;
			}
			if(model == _model)
			{
				std::fprintf(stderr, "[MD/MM] official OS update discovered at %s\n",
					file.c_str());
				return Rom(converted, file);
			}
		}
		return {};
	}

	bool RomLoader::isRomForModel(const std::vector<uint8_t>& _data,
		const MachineModel _model)
	{
		if(_data.size() != g_romSize)
			return false;
		MachineModel updateModel = MachineModel::Machinedrum;
		if(firmwareUpdate::model(_data, updateModel))
			return updateModel == _model;

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
