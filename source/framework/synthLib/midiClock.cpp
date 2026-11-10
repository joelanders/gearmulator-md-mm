#include "midiClock.h"
#include "midiTypes.h"
#include "plugin.h"

#include <algorithm>
#include <cmath>

namespace synthLib
{
	static constexpr double ClockTicksPerQuarter = 24.0;

	void MidiClock::process(const double _bpm, const double _ppqPos, const bool _isPlaying,
		const size_t _sampleCount, const bool _ppqKnown)
	{
		// Hosts may omit BPM when stopping. Transport edges must still reach the
		// instrument; stopped transport intentionally does not generate clocks.
		if(!_isPlaying)
		{
			if(m_isPlaying) stop();
			return;
		}
		if(!_sampleCount) return;
		const double rate = m_plugin.getHostSamplerate();
		if(!std::isfinite(rate) || rate <= 0) return;
		if(std::isfinite(_bpm) && _bpm > 0 && _bpm <= rate * 60 / ClockTicksPerQuarter)
			m_lastBpm = _bpm;
		if(m_lastBpm <= 0) return;

		const auto quartersPerSample = m_lastBpm / (60.0 * rate);
		const auto samplesPerClock = rate * 60.0 / (m_lastBpm * ClockTicksPerQuarter);
		const bool positionKnown = _ppqKnown && std::isfinite(_ppqPos) && std::abs(_ppqPos) < 1e12;
		const auto ppq = positionKnown ? _ppqPos : (m_isPlaying ? m_expectedPpq : 0.0);
		// Allow at most two samples of host rounding. Even within this tolerance
		// the pulse offsets use current PPQ; the tolerance only avoids false
		// STOP/CONTINUE edges. A loop or seek relocates before any new clock.
		if(m_isPlaying && positionKnown
			&& std::abs(ppq - m_expectedPpq) > std::max(2 * quartersPerSample, 1e-8))
			stop();
		if(!m_isPlaying) start(ppq);

		for(;; ++m_nextClockTick)
		{
			const auto distance = (static_cast<double>(m_nextClockTick) - ppq * ClockTicksPerQuarter) * samplesPerClock;
			// Remove only floating-point noise around exact sample boundaries.
			// No rounded block lengths or per-sample phase accumulation are used.
			const auto offset = std::max(0.0, std::ceil(distance - 1e-7));
			if(offset >= static_cast<double>(_sampleCount)) break;
			m_plugin.insertMidiEvent({MidiEventSource::Internal, M_TIMINGCLOCK, 0, 0,
				static_cast<uint32_t>(offset)});
		}
		m_expectedPpq = ppq + static_cast<double>(_sampleCount) * quartersPerSample;
	}

	void MidiClock::restart()
	{
		stop();
	}

	void MidiClock::start(const double _ppqPos)
	{
		m_nextClockTick = static_cast<int64_t>(std::ceil(_ppqPos * ClockTicksPerQuarter - 1e-9));
		m_isPlaying = true;
		if(_ppqPos <= 0)
			m_plugin.insertMidiEvent({MidiEventSource::Internal, M_START});
		else
		{
			// MIDI SPP has six clocks per unit (one sixteenth note), and a
			// 14-bit range. Clock phase remains at the host's 24-PPQN position.
			const auto position = static_cast<uint32_t>(std::fmod(std::floor(_ppqPos * 4), 16384.0));
			m_plugin.insertMidiEvent({MidiEventSource::Internal, M_SONGPOSITION,
				static_cast<uint8_t>(position & 127), static_cast<uint8_t>(position >> 7)});
			m_plugin.insertMidiEvent({MidiEventSource::Internal, M_CONTINUE});
		}
	}

	void MidiClock::stop()
	{
		m_isPlaying = false;
		m_plugin.insertMidiEvent({MidiEventSource::Internal, M_STOP});
	}
}
