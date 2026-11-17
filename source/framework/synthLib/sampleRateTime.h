#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace synthLib
{
	// Absolute positions avoid accumulating one rounding error per callback.
	// Integer audio rates also need integer division: reciprocal multiplication
	// under fast-math can put an exact sample boundary just above an integer.
	inline uint64_t rescaleSamplesCeil(uint64_t sample, float from, float to)
	{
		const auto source = static_cast<uint32_t>(from);
		const auto destination = static_cast<uint32_t>(to);
		if(source && from == static_cast<float>(source) && to == static_cast<float>(destination))
		{
			const auto seconds = sample / source;
			const auto fraction = ((sample % source) * destination + source - 1) / source;
			const auto maximum = std::numeric_limits<uint64_t>::max();
			if(destination && seconds > (maximum - fraction) / destination)
				return maximum;
			return seconds * destination + fraction;
		}
		return static_cast<uint64_t>(std::ceil(static_cast<double>(sample) * to / from));
	}
}
