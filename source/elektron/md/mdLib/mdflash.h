#pragma once

#include <cstdint>
#include <optional>

namespace md
{
	// AMD-compatible command interface used by the Machinedrum's 8 MiB NOR
	// flash. The Microcontroller owns the bytes and applies returned mutations.
	class FlashCommandDecoder
	{
	public:
		struct Operation
		{
			enum class Type : uint8_t { ProgramWord, EraseSector };
			Type type;
			uint32_t offset;
			uint16_t value;
		};

		static constexpr uint16_t g_manufacturerId = 0x0020;
		static constexpr uint16_t g_deviceId = 0x22fd;
		static constexpr uint32_t g_sectorSize = 0x10000;

		std::optional<Operation> write16(uint32_t _offset, uint16_t _value);
		std::optional<uint16_t> read16(uint32_t _offset) const;
		std::optional<uint8_t> read8(uint32_t _offset) const;

	private:
		enum class State : uint8_t
		{
			ReadArray,
			Unlock1,
			Unlock2,
			Identify,
			Program,
			EraseUnlock1,
			EraseUnlock2,
			EraseCommand
		};

		State m_state = State::ReadArray;
	};
}
