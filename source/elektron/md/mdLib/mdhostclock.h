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

	// Latest DSP cycle at or before an elapsed host timestamp. Subtract the
	// integral boot origin before converting: floating frame coordinates can
	// round an exact deadline down by one cycle, differently with host FMA.
	template<uint64_t HostHz, uint64_t DspHz>
	constexpr uint64_t dspCatchupDeadline(const uint64_t _dspOrigin,
		const uint64_t _hostElapsed)
	{
		constexpr auto max = std::numeric_limits<uint64_t>::max();
		static_assert(HostHz > 0 && DspHz > 0, "clock rates must be positive");
		static_assert(HostHz - 1 <= max / DspHz, "fractional conversion must fit in 64 bits");
		const auto seconds = _hostElapsed / HostHz;
		if(seconds > (max - _dspOrigin) / DspHz)
			return max;
		const auto whole = _dspOrigin + seconds * DspHz;
		const auto fraction = ((_hostElapsed % HostHz) * DspHz) / HostHz;
		return fraction > max - whole ? max : whole + fraction;
	}

}
