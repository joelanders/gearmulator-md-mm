#pragma once

#include <cstddef>
#include <cstdint>

namespace synthLib
{
	class Plugin;

	class MidiClock
	{
	public:
		explicit MidiClock(Plugin& _plugin) : m_plugin(_plugin) {}

		void process(double _bpm, double _ppqPos, bool _isPlaying, size_t _sampleCount, bool _ppqKnown = true);

		void restart();

	private:
		void stop();
		void start(double _ppqPos);

		Plugin& m_plugin;

		bool m_isPlaying = false;
		int64_t m_nextClockTick = 0;
		double m_expectedPpq = 0.0;
		double m_lastBpm = 0.0;
	};
}
