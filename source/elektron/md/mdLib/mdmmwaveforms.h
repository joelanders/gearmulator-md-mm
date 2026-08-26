#pragma once

#include <cstdint>
#include <vector>

namespace md::mmwaveforms
{
	inline constexpr uint32_t g_sourceBase = 0x100000;
	inline constexpr uint32_t g_recordBytes = 0x17fc;
	inline constexpr uint32_t g_headerBytes = 8;
	inline constexpr uint32_t g_waveformCount = 64;
	inline constexpr uint32_t g_wordsPerWave = 2044;
	inline constexpr uint32_t g_destinationBase = 0x150000;
	inline constexpr uint32_t g_destinationStride = 0x800;

	// Supported MKII images store the factory DigiPRO bank as 64 fixed-size
	// records. Each record contains an eight-byte header followed by 2,044
	// big-endian 24-bit waveform words. Waveform slots are zero-padded to 2,048
	// words.
	template<typename WriteWord>
	bool loadFactoryBank(const std::vector<uint8_t>& _rom, WriteWord&& _writeWord)
	{
		if(_rom.size() < g_sourceBase + g_waveformCount * g_recordBytes)
			return false;
		for(uint32_t wave = 0; wave < g_waveformCount; ++wave)
		{
			const auto record = g_sourceBase + wave * g_recordBytes;
			if(_rom[record] != 0x1a || _rom[record + 1] != wave)
				return false;
		}

		for(uint32_t wave = 0; wave < g_waveformCount; ++wave)
		{
			const auto source = g_sourceBase + wave * g_recordBytes + g_headerBytes;
			const auto destination = g_destinationBase + wave * g_destinationStride;
			for(uint32_t word = 0; word < g_wordsPerWave; ++word)
			{
				const auto offset = source + word * 3;
				const uint32_t value = static_cast<uint32_t>(_rom[offset]) << 16
					| static_cast<uint32_t>(_rom[offset + 1]) << 8
					| static_cast<uint32_t>(_rom[offset + 2]);
				_writeWord(destination + word, value);
			}
		}
		return true;
	}
}
