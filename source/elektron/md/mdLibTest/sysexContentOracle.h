#pragma once

#include "mdLib/mdhardware.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

namespace md::test
{
	using Sysex = std::vector<uint8_t>;
	inline std::vector<Sysex> splitSysex(const Sysex& bytes)
	{
		std::vector<Sysex> messages;
		for(auto begin = bytes.begin(); begin != bytes.end();)
		{
			const auto end = std::find(begin, bytes.end(), uint8_t{0xf7});
			if(*begin != 0xf0 || end == bytes.end()) return {};
			messages.emplace_back(begin, end + 1);
			begin = end + 1;
		}
		return messages;
	}

	// Independent implementation of Elektron's documented MSB-first packing.
	inline Sysex unpackElektron(Sysex::const_iterator begin, Sysex::const_iterator end)
	{
		Sysex result;
		while(begin != end)
		{
			const auto high = *begin++;
			for(int bit = 6; bit >= 0 && begin != end; --bit, ++begin)
				result.push_back(*begin | (((high >> bit) & 1) << 7));
		}
		return result;
	}

	inline bool verifyDigiProContents(const Sysex& file, const Sysex& flash)
	{
		std::map<uint8_t, Sysex> waves;
		for(const auto& message : splitSysex(file))
			if(message.size() > 19 && message[6] == 0x5d)
				waves[message[9]] = unpackElektron(message.begin() + 14, message.end() - 5);
		if(waves.empty()) return false;
		for(const auto& [slot, wave] : waves)
		{
			const auto found = std::search(flash.begin(), flash.end(), wave.begin(), wave.end());
			if(wave.empty() || found == flash.end())
			{
				std::printf("DigiPRO content missing: slot=%u bytes=%zu\n", slot, wave.size());
				if(wave.empty()) return false;
				const auto prefix = std::search(flash.begin(), flash.end(), wave.begin(), wave.begin() + std::min<size_t>(64, wave.size()));
				if(prefix != flash.end())
				{
					size_t count = 0, first = 0;
					for(size_t i = 0; i < wave.size() && prefix + i != flash.end(); ++i)
						if(prefix[i] != wave[i]) { if(!count) first = i; ++count; }
					std::printf("Wave prefix at %zx firstDifference=%zu differences=%zu expected=%02x actual=%02x\n",
						size_t(prefix-flash.begin()), first, count, wave[first], prefix[first]);
				}
				return false;
			}
			std::printf("Verified DigiPRO slot=%u waveformBytes=%zu flashOffset=%zx\n", slot, wave.size(), size_t(found-flash.begin()));
		}
		return true;
	}

	inline Sysex requestDump(Hardware& hardware, const Sysex& expected)
	{
		std::vector<synthLib::SMidiEvent> events;
		hardware.readMidiOut(events);
		synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
		request.sysex = {0xf0, 0, 0x20, 0x3c, expected[4], 0,
			uint8_t(expected[6] + 1), expected[9], 0xf7};
		if(!hardware.sendMidi(request)) return {};
		for(size_t i = 0; i < g_samplerate * 5 / 64; ++i)
		{
			hardware.advance(64);
			events.clear();
			hardware.readMidiOut(events);
			for(const auto& event : events)
				if(event.sysex.size() > 14 && event.sysex[4] == expected[4]
					&& event.sysex[6] == expected[6] && event.sysex[9] == expected[9]) return event.sysex;
		}
		return {};
	}

	inline bool verifyDumpContents(Hardware& hardware, const Sysex& file)
	{
		std::map<unsigned, Sysex> finalSlots;
		for(const auto& message : splitSysex(file))
			if(message.size() > 14)
				finalSlots[(unsigned(message[6]) << 8) | message[9]] = message;
		if(finalSlots.empty()) return false;
		bool success = true;
		for(const auto& [key, expected] : finalSlots)
		{
			const auto actual = requestDump(hardware, expected);
			// OS 1.63 expands legacy 32-step MD patterns with a 64-step tail.
			// Compare every supplied payload byte; do not call the unsupplied tail
			// verified. All other message shapes must have exactly matching sizes.
			const bool expandedPattern = expected[4] == 2 && expected[6] == 0x67
				&& expected.size() == 2763 && actual.size() == 5410;
			if((actual.size() != expected.size() && !expandedPattern)
				|| actual.size() < expected.size()
				|| !std::equal(expected.begin() + 10, expected.end() - 5, actual.begin() + 10))
			{
				size_t first = 10, differences = 0;
				for(size_t i = 10; i + 5 < std::min(actual.size(), expected.size()); ++i)
					if(actual[i] != expected[i]) { if(!differences) first = i; ++differences; }
				std::printf("Dump mismatch: command=%02x version=%u.%u slot=%u expectedSize=%zu actualSize=%zu firstDifference=%zu differences=%zu\n",
					expected[6], expected[7], expected[8], expected[9], expected.size(), actual.size(), first, differences);
				success = false;
			}
		}
		std::printf("Firmware dump readback: slots=%zu match=%u\n", finalSlots.size(), success);
		return success;
	}
}
