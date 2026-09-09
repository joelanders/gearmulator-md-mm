#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp56kBase/ringbuffer.h"
#include "dsp56kEmu/audio.h"
#include "synthLib/audioTypes.h"

namespace md
{
	template<size_t Channels, size_t Capacity>
	class HostAudioQueue
	{
	public:
		using Frame = std::array<dsp56k::TWord, Channels>;

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

		size_t drop(const size_t _count)
		{
			const auto dropped = std::min(_count, m_frames.size());
			for(size_t i = 0; i < dropped; ++i)
				m_frames.pop_front();
			return dropped;
		}

		void clear()
		{
			drop(m_frames.size());
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

	using RealtimeHostAudioQueue = HostAudioQueue<6, dsp56k::Audio::RingBufferSize>;
	using RealtimeHostAudioInputQueue = HostAudioQueue<2, dsp56k::Audio::RingBufferSize * 2>;

	inline void mapCodecOutputFrame(RealtimeHostAudioQueue::Frame& _hostFrame,
		const dsp56k::Audio::TxFrame& _codecFrame)
	{
		_hostFrame.fill(0);
		const auto slots = std::min<size_t>(_codecFrame.size(), 2);
		for(size_t slot = 0; slot < slots; ++slot)
		{
			for(size_t transmitter = 0; transmitter < 3; ++transmitter)
				_hostFrame[transmitter * 2 + slot]
					= _codecFrame[slot][transmitter];
		}
	}

	template<size_t Capacity>
	size_t appendHostAudioInput(HostAudioQueue<2, Capacity>& _queue,
		const synthLib::TAudioInputs& _inputs, const uint32_t _sourceFrames,
		const uint32_t _sourceOffset, const uint32_t _frames)
	{
		size_t dropped = 0;
		for(uint32_t i = 0; i < _frames; ++i)
		{
			typename HostAudioQueue<2, Capacity>::Frame frame{};
			const auto sourceIndex = _sourceOffset + i;
			for(size_t channel = 0; channel < frame.size(); ++channel)
			{
				if(_inputs[channel] && sourceIndex < _sourceFrames)
					frame[channel] = dsp56k::sample2dsp(_inputs[channel][sourceIndex]);
			}
			if(_queue.push(frame))
				++dropped;
		}
		return dropped;
	}

	// ADC samples belong to machine time even while a receiver is stopped. A
	// plain FIFO preserves the boot interval as a permanent, unreported delay.
	template<size_t Capacity>
	class HostAudioInputTimeline
	{
	public:
		using Frame = typename HostAudioQueue<2, Capacity>::Frame;
		void reset(const int64_t _firstFrame)
		{
			m_queue.clear();
			m_frontFrame = m_startFrame = _firstFrame;
		}
		size_t append(const synthLib::TAudioInputs& _inputs, const uint32_t _sourceFrames,
			const uint32_t _sourceOffset, const uint32_t _frames, const int64_t _firstFrame)
		{
			if(m_frontFrame + static_cast<int64_t>(m_queue.size()) != _firstFrame)
				reset(_firstFrame); // machine time advanced without host input
			const auto dropped = appendHostAudioInput(m_queue, _inputs, _sourceFrames, _sourceOffset, _frames);
			m_frontFrame += static_cast<int64_t>(dropped);
			return dropped;
		}
		bool readAt(const int64_t _frame, Frame& _result)
		{
			if(_frame < m_frontFrame)
				return false;
			const auto skip = static_cast<size_t>(std::min<uint64_t>(
				static_cast<uint64_t>(_frame - m_frontFrame), m_queue.size()));
			m_frontFrame += static_cast<int64_t>(m_queue.drop(skip));
			if(m_frontFrame != _frame || !m_queue.pop(_result))
				return false;
			++m_frontFrame;
			return true;
		}
		bool beforeStart(const int64_t _frame) const { return _frame < m_startFrame; }
		size_t size() const { return m_queue.size(); }
	private:
		HostAudioQueue<2, Capacity> m_queue;
		int64_t m_frontFrame = 0, m_startFrame = 0;
	};
	using RealtimeHostAudioInputTimeline = HostAudioInputTimeline<RealtimeHostAudioInputQueue::capacity()>;

	// Drain a scheduler-owned codec queue into six host channels while advancing
	// the machine by the full requested duration. Existing carry is always older
	// than newly generated audio and is therefore copied first. Splitting oversized
	// offline blocks keeps fixed storage bounded without changing machine time.
	template<size_t Capacity, typename Advance>
	size_t renderHostAudio(HostAudioQueue<6, Capacity>& _queue,
		std::array<std::vector<dsp56k::TWord>, 6>& _outputs,
		const uint32_t _frames, Advance&& _advance)
	{
		typename HostAudioQueue<6, Capacity>::Frame frame{};
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
