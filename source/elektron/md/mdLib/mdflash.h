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
		// M29W640FB bottom-boot geometry: the first 64 KiB is split into eight
		// 8 KiB boot/parameter blocks; the remaining erase blocks are 64 KiB.
		static constexpr uint32_t g_bootSectorLimit = 0x10000;
		static constexpr uint32_t g_bootSectorSize = 0x2000;
		static constexpr uint32_t g_sectorSize = 0x10000;
		static constexpr uint32_t eraseSectorSize(const uint32_t _offset)
		{
			return _offset < g_bootSectorLimit ? g_bootSectorSize : g_sectorSize;
		}
		static constexpr uint32_t eraseSectorBegin(const uint32_t _offset)
		{
			const auto size = eraseSectorSize(_offset);
			return _offset & ~(size - 1);
		}

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
