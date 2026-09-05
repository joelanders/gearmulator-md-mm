#pragma once

#include <cstdint>
#include <limits>

namespace md
{
	// Convert an elapsed DSP clock to the first host cycle at or after it.
	// Splitting whole seconds from the remainder avoids multiplying the full
	// 64-bit timestamp. Keep the boot origin integral, too: rounding a large
	// floating-point sum can erase the fraction before a final ceil sees it.
	template<uint64_t HostHz, uint64_t DspHz>
	constexpr uint64_t hostReceiveDeadline(const uint64_t _hostOrigin,
		const uint64_t _dspElapsed)
	{
		constexpr auto max = std::numeric_limits<uint64_t>::max();
		static_assert(HostHz > 0 && HostHz <= DspHz, "host clock must not exceed DSP clock");
		static_assert(DspHz - 1 <= max / HostHz, "fractional conversion must fit in 64 bits");
		const auto fraction = (_dspElapsed % DspHz) * HostHz;
		const auto elapsed = (_dspElapsed / DspHz) * HostHz
			+ fraction / DspHz + (fraction % DspHz != 0);
		// Saturate an unrepresentable future deadline instead of wrapping into
		// the past. For HostHz <= DspHz, elapsed itself never exceeds the input.
		return elapsed > max - _hostOrigin ? max : _hostOrigin + elapsed;
	}
}
