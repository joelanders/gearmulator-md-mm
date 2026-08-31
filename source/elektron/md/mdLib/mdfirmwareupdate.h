#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mdtypes.h"

namespace md::firmwareUpdate
{
	// Gearmulator's 8 MiB update image is a private, deterministic container for
	// the payloads in an official Machinedrum/Monomachine OS SysEx file. It is not
	// an Elektron flash dump and is not intended to be written to hardware.
	enum class Section : uint32_t
	{
		MainOs = 0,
		DspMixer = 1,
		DspProducer = 2,
		DspCommon = 3,
		FactoryData = 4
	};

	bool convertSysexToRom(const std::vector<uint8_t>& _sysex,
		std::vector<uint8_t>& _rom, MachineModel& _model, std::string& _error);

	bool isConvertedRom(const std::vector<uint8_t>& _rom);
	bool model(const std::vector<uint8_t>& _rom, MachineModel& _model);
	bool factoryFlashAddress(const std::vector<uint8_t>& _rom, uint32_t& _address);
	bool readSection(const std::vector<uint8_t>& _rom, Section _section,
		std::vector<uint8_t>& _data);

	// Validate and decode one little-endian 24-bit DSP command stream. Each
	// callback receives a memory-space command (0=P, 1=X, 2=Y), destination,
	// and its words. The leading and trailing execute descriptors are validated
	// and returned through _entry rather than passed to the callback.
	using DspWrite = bool(*)(void* _context, uint32_t _space, uint32_t _address,
		const uint32_t* _words, size_t _count);
	bool visitDspCommands(const std::vector<uint8_t>& _section, DspWrite _write,
		void* _context, uint32_t& _entry, std::string& _error);
}
