#pragma once

#include <cstdint>
#include <vector>

namespace md::test
{
	// Generated test audio only; no downloaded bank or firmware is redistributed.
	inline std::vector<uint8_t> sdsSample(uint32_t _words = 5201, uint8_t _bits = 16,
		uint8_t _device = 0, uint8_t _slot = 0)
	{
		std::vector<uint8_t> bytes{0xf0, 0x7e, _device, 1, _slot, 0, _bits};
		const auto value = [&](uint32_t v)
		{
			for(size_t i = 0; i < 3; ++i) { bytes.push_back(v & 0x7f); v >>= 7; }
		};
		value(31250); // 32 kHz, avoids receiver resampling.
		value(_words);
		value(0);
		value(_words - 1);
		bytes.push_back(0x7f);
		bytes.push_back(0xf7);
		const uint8_t name[] = {0xf0, 0, 0x20, 0x3c, 2, 0, 0x73, _slot, 'T', 'E', 'S', 'T', 0xf7};
		bytes.insert(bytes.end(), std::begin(name), std::end(name));
		const size_t bytesPerWord = (_bits + 6) / 7;
		const size_t wordsPerPacket = 120 / bytesPerWord;
		for(size_t first = 0, packet = 0; first < _words; first += wordsPerPacket, ++packet)
		{
			const auto offset = bytes.size();
			bytes.insert(bytes.end(), {0xf0, 0x7e, _device, 2, uint8_t(packet & 0x7f)});
			for(size_t i = 0; i < wordsPerPacket; ++i)
			{
				// Distinct periodic ramp with exactly representable 12-bit values.
				const uint32_t period = _slot == 0 ? 97 : 151;
				const uint32_t scale = _slot == 0 ? 40 : 25;
				const uint32_t word = first + i < _words
					? uint32_t(((first + i) % period) * scale + 100) << 16 : 0;
				for(size_t j = 0; j < bytesPerWord; ++j)
					bytes.push_back((word >> (21 - j * 7)) & 0x7f);
			}
			uint8_t checksum = 0;
			for(size_t i = offset + 1; i < bytes.size(); ++i) checksum ^= bytes[i];
			bytes.push_back(checksum);
			bytes.push_back(0xf7);
		}
		return bytes;
	}
}
