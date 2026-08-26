#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp56kBase/ringbuffer.h"
#include "dsp56kEmu/audio.h"

namespace md
{
	template<size_t Capacity>
	class HostAudioQueue
	{
	public:
		using Frame = std::array<dsp56k::TWord, 6>;

		template<typename Fill>
		bool emplace(const Fill& _fill)
		{
			const bool dropped = m_frames.full();
			if(dropped)
				m_frames.pop_front();
			m_frames.emplace_back(_fill);
			return dropped;
		}

		bool push(const Frame& _frame)
		{
			return emplace([&_frame](Frame& _destination) { _destination = _frame; });
		}

		bool pop(Frame& _frame)
		{
			if(m_frames.empty())
				return false;
			_frame = m_frames.pop_front();
			return true;
		}

		size_t trimTo(const size_t _maximum)
		{
			const size_t dropped = m_frames.size() > _maximum
				? m_frames.size() - _maximum : 0;
			for(size_t i = 0; i < dropped; ++i)
				m_frames.pop_front();
			return dropped;
		}

		bool empty() const { return m_frames.empty(); }
		size_t size() const { return m_frames.size(); }
		static constexpr size_t capacity() { return Capacity; }

	private:
		dsp56k::RingBuffer<Frame, Capacity, false, false> m_frames;
	};

	using RealtimeHostAudioQueue = HostAudioQueue<dsp56k::Audio::RingBufferSize>;

	// Drain a scheduler-owned codec queue into six host channels while advancing
	// the machine by the full requested duration. Existing carry is always older
	// than newly generated audio and is therefore copied first. Splitting oversized
	// offline blocks keeps fixed storage bounded without changing machine time.
	template<size_t Capacity, typename Advance>
	size_t renderHostAudio(HostAudioQueue<Capacity>& _queue,
		std::array<std::vector<dsp56k::TWord>, 6>& _outputs,
		const uint32_t _frames, Advance&& _advance)
	{
		typename HostAudioQueue<Capacity>::Frame frame{};
		uint32_t outputOffset = 0;
		while(outputOffset < _frames)
		{
			const auto chunk = std::min<uint32_t>(_frames - outputOffset,
				static_cast<uint32_t>(Capacity));
			uint32_t chunkOutput = 0;
			const auto copyAvailable = [&]
			{
				while(chunkOutput < chunk && _queue.pop(frame))
				{
					for(uint32_t channel = 0; channel < frame.size(); ++channel)
						_outputs[channel][outputOffset + chunkOutput] = frame[channel];
					++chunkOutput;
				}
			};
			copyAvailable();
			_advance(chunk);
			copyAvailable();
			outputOffset += chunk;
		}

		return _queue.trimTo(std::min<size_t>(_frames, Capacity));
	}
}
