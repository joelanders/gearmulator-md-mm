#pragma once

#include "mdrom.h"

#include "synthLib/romLoader.h"

namespace md
{
	class RomLoader : synthLib::RomLoader
	{
	public:
		static Rom findROM();
		static Rom findROM(MachineModel _model);
		static bool isSupportedImage(size_t _size, uint64_t _fingerprint,
			MachineModel _model);
		static bool isRomForModel(const std::vector<uint8_t>& _data, MachineModel _model);
	};
}
