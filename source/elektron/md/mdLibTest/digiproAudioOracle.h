#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace md::test
{
	// Independent of flash placement and DSP implementation. Public DigiPRO banks
	// unpack to 6132 bytes. Their first 1024 signed, big-endian 24-bit values form
	// the full-resolution cycle, as verified against DDRW audio at MIDI note 48.
	// The remaining resolution levels are covered by byte persistence, not modeled
	// here. This is an empirical playback oracle, not a complete oscillator spec.
	inline std::vector<double> digiProCycle(const std::vector<uint8_t>& wave)
	{
		if(wave.size() != 6132) return {};
		std::vector<double> reference(1024);
		for(size_t i = 0; i < reference.size(); ++i)
		{
			const uint32_t raw = (uint32_t(wave[i*3]) << 16) | (uint32_t(wave[i*3+1]) << 8) | wave[i*3+2];
			reference[i] = double(int64_t(raw ^ 0x800000u) - 0x800000) / 0x800000;
		}
		return reference;
	}
	inline double digiProCorrelation(const std::vector<float>& audio, const std::vector<uint8_t>& wave)
	{
		const auto reference = digiProCycle(wave);
		constexpr size_t window = 4096, start = 4096;
		if(reference.empty() || audio.size() < start + window
			|| !std::all_of(audio.begin(), audio.end(), [](float a) { return std::isfinite(a); })) return -1;
		const auto points = reference.size();
		const double frequency = 440.0 * std::pow(2.0, (48.0 - 69.0) / 12.0);
		double best = -1;
		for(size_t phase = 0; phase < points; ++phase)
		{
			double x = 0, y = 0, xx = 0, yy = 0, xy = 0;
			for(size_t i = 0; i < window; ++i)
			{
				const double position = std::fmod(phase + i * frequency * points / 44100.0, double(points));
				const auto lower = size_t(position);
				const auto a = reference[lower] + (reference[(lower + 1) % points] - reference[lower]) * (position - lower);
				const double b = audio[start + i];
				x += a; y += b; xx += a*a; yy += b*b; xy += a*b;
			}
			const double variance = (xx - x*x/window) * (yy - y*y/window);
			if(variance > 0) best = std::max(best, (xy-x*y/window)/std::sqrt(variance));
		}
		return best;
	}
	inline bool digiProAudioMatches(const std::vector<float>& audio, const std::vector<uint8_t>& wave)
	{
		double energy = 0;
		for(auto sample : audio) energy += double(sample) * sample;
		return energy > 0.0001 && digiProCorrelation(audio, wave) > 0.90;
	}
}
