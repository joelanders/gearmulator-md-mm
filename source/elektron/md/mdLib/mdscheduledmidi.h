#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "synthLib/midiTypes.h"

namespace md
{
	// First CPU cycle at or after a native sample. Convert the absolute position,
	// carrying the rational remainder instead of rounding every host block.
	template<uint64_t CpuHz, uint64_t SampleHz>
	constexpr uint64_t midiReceiveDeadline(const uint64_t _sample)
	{
		constexpr auto maximum = std::numeric_limits<uint64_t>::max();
		static_assert(CpuHz > 0 && SampleHz > 0);
		static_assert(SampleHz - 1 <= maximum / CpuHz);
		const auto seconds = _sample / SampleHz;
		if(seconds > maximum / CpuHz)
			return maximum;
		const auto whole = seconds * CpuHz;
		const auto remainder = (_sample % SampleHz) * CpuHz;
		const auto fraction = remainder / SampleHz + (remainder % SampleHz != 0);
		return fraction > maximum - whole ? maximum : whole + fraction;
	}

	// Owned entirely by the audio/emulation thread. The storage is reserved at
	// construction, and ordinary MIDI never allocates or waits. Sorting deadlines
	// handles unsorted input; retiming retains the original sample and wire order.
	template<size_t Capacity>
	class ScheduledMidiQueue
	{
	public:
		struct Entry
		{
			synthLib::SMidiEvent event;
			uint64_t cycle;
			uint64_t order;
			uint64_t sample; // Native position before extra latency, never rebased.
		};

		ScheduledMidiQueue() { m_events.reserve(Capacity); }

		bool push(const synthLib::SMidiEvent& _event, const uint64_t _cycle, const uint64_t _sample = 0)
		{
			if(m_events.size() == Capacity)
				return false;
			m_events.push_back({_event, _cycle, m_order++, _sample});
			std::push_heap(m_events.begin(), m_events.end(), later);
			return true;
		}

		template<typename Deadline>
		void retime(const Deadline& _deadline)
		{
			for(auto& entry : m_events)
				entry.cycle = _deadline(entry.sample);
			std::make_heap(m_events.begin(), m_events.end(), later);
		}

		bool empty() const { return m_events.empty(); }
		size_t size() const { return m_events.size(); }
		const Entry& front() const { return m_events.front(); }
		bool ready(const uint64_t _cycle) const
		{
			return !empty() && front().cycle <= _cycle;
		}
		void pop()
		{
			std::pop_heap(m_events.begin(), m_events.end(), later);
			m_events.pop_back();
		}

	private:
		static bool later(const Entry& _a, const Entry& _b)
		{
			return _a.cycle != _b.cycle ? _a.cycle > _b.cycle : _a.order > _b.order;
		}
		std::vector<Entry> m_events;
		uint64_t m_order = 0;
	};
}
