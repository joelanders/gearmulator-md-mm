#pragma once

#include "mdromdata.h"
#include "mdtypes.h"

namespace md
{
	class Rom : public RomData<g_romSize>
	{
	public:
		Rom();
		Rom(const std::string& _filename);
		Rom(const std::vector<uint8_t>& _data, const std::string& _filename);

		static bool isValidRom(const std::vector<uint8_t>& _data);
	};
}
