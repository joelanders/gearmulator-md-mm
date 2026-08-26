#include "mdrom.h"

namespace md
{
	Rom::Rom()
	{
		if(!isValidRom(data()))
			invalidate();
	}

	Rom::Rom(const std::string& _filename) : RomData(_filename)
	{
		if(!isValidRom(data()))
			invalidate();
	}

	Rom::Rom(const std::vector<uint8_t>& _data, const std::string& _filename) : RomData(_data, _filename)
	{
		if(!isValidRom(data()))
			invalidate();
	}

	bool Rom::isValidRom(const std::vector<uint8_t>& _data)
	{
		// Both supported products use an 8 MiB image.
		return _data.size() == g_romSize;
	}
}
