#include "mdLib/mdhostaudioqueue.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void verifyHostAudioInputLookAhead()
	{
		md::HostAudioQueue<2, 16> queue;
		const md::HostAudioQueue<2, 16>::Frame silence{};
		for(uint32_t i = 0; i < 3; ++i)
			queue.push(silence);

		const std::array<float, 8> left{
			0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.07f, 0.08f};
		const std::array<float, 8> right{
			-0.01f, -0.02f, -0.03f, -0.04f, -0.05f, -0.06f, -0.07f, -0.08f};
		const synthLib::TAudioInputs inputs{
			left.data(), right.data(), nullptr, nullptr};
		require(md::appendHostAudioInput(queue, inputs, left.size(), 0, 4) == 0,
			"host input unexpectedly overflowed");

		md::HostAudioQueue<2, 16>::Frame frame{};
		for(uint32_t i = 0; i < 3; ++i)
			require(queue.pop(frame) && frame[0] == 0 && frame[1] == 0,
				"host input look-ahead did not begin with silence");
		for(uint32_t sample = 0; sample < 2; ++sample)
			require(queue.pop(frame)
				&& frame[0] == dsp56k::sample2dsp(left[sample])
				&& frame[1] == dsp56k::sample2dsp(right[sample]),
				"host input changed during simulated scheduler overshoot");

		require(md::appendHostAudioInput(queue, inputs, left.size(), 4, 4) == 0,
			"host input continuation unexpectedly overflowed");
		for(uint32_t sample = 2; sample < left.size(); ++sample)
			require(queue.pop(frame)
				&& frame[0] == dsp56k::sample2dsp(left[sample])
				&& frame[1] == dsp56k::sample2dsp(right[sample]),
				"host input continuity was lost across callback blocks");
		require(queue.empty(), "host input queue retained unexpected samples");

		md::HostAudioQueue<2, 4> bounded;
		require(md::appendHostAudioInput(bounded, inputs, left.size(), 0, 6) == 2,
			"host input overflow telemetry did not count dropped frames");
	}

	void verifyHostAudioInputTimeline()
	{
		md::HostAudioInputTimeline<16> timeline;
		std::array<float,16> samples{};
		for(unsigned i=0;i<samples.size();++i) samples[i]=(i+1)/32.0f;
		synthLib::TAudioInputs inputs{}; inputs[0]=samples.data();
		md::HostAudioInputTimeline<16>::Frame frame{};
		constexpr int64_t origin=44100ll*86400*2;
		timeline.reset(origin);
		require(timeline.append(inputs,16,0,12,origin)==0, "timeline append overflowed");
		require(timeline.beforeStart(origin-1) && !timeline.readAt(origin-1,frame),
			"pre-start silence consumed a real input frame");
		// A receiver starting late reads the current ADC sample, not the first
		// buffered sample from firmware boot. Future frames remain available.
		require(timeline.readAt(origin+9,frame) && frame[0]==dsp56k::sample2dsp(samples[9]),
			"ADC startup retained a stale FIFO backlog");
		require(timeline.size()==2 && timeline.readAt(origin+10,frame)
			&& frame[0]==dsp56k::sample2dsp(samples[10]), "ADC timeline lost continuity");
		require(timeline.append(inputs,16,12,4,origin+12)==0, "contiguous append failed");
		require(timeline.readAt(origin+15,frame) && frame[0]==dsp56k::sample2dsp(samples[15]),
			"ADC position drifted across append boundaries");
		require(!timeline.readAt(origin+16,frame), "ADC invented unavailable future input");
		require(timeline.append(inputs,16,0,4,origin+100)==0
			&& timeline.beforeStart(origin+99), "discontinuous host time retained old audio");
		require(timeline.readAt(origin+100,frame) && frame[0]==dsp56k::sample2dsp(samples[0]),
			"ADC did not resume at the new input origin");
	}

	void verifyHostAudioOutputRouting()
	{
		dsp56k::Audio::TxFrame codecFrame;
		codecFrame.resize(2);
		codecFrame[0] = {10, 20, 30};
		codecFrame[1] = {11, 21, 31};
		md::RealtimeHostAudioQueue::Frame mappedFrame{};
		md::mapCodecOutputFrame(mappedFrame, codecFrame);
		require(mappedFrame == md::RealtimeHostAudioQueue::Frame{
			10, 11, 20, 21, 30, 31},
			"codec slots and transmitters were mapped to the wrong host channels");

		codecFrame.resize(1);
		md::mapCodecOutputFrame(mappedFrame, codecFrame);
		require(mappedFrame == md::RealtimeHostAudioQueue::Frame{
			10, 0, 20, 0, 30, 0},
			"missing codec slot did not map to silent host channels");

		md::HostAudioQueue<6, 8> queue;
		const auto pushFrame = [&queue](const dsp56k::TWord _sample)
		{
			queue.emplace([_sample](md::HostAudioQueue<6, 8>::Frame& _frame)
			{
				for(size_t channel = 0; channel < _frame.size(); ++channel)
					_frame[channel] = _sample
						+ static_cast<dsp56k::TWord>(channel * 100);
			});
		};
		pushFrame(1);
		pushFrame(2);

		std::array<std::vector<dsp56k::TWord>, 6> outputs;
		for(auto& output : outputs)
			output.resize(5);
		dsp56k::TWord generated = 3;
		md::renderHostAudio(queue, outputs, 5, [&](const uint32_t _frames)
		{
			for(uint32_t i = 0; i < _frames; ++i)
				pushFrame(generated++);
		});

		for(size_t channel = 0; channel < outputs.size(); ++channel)
			for(dsp56k::TWord frameIndex = 0; frameIndex < 5; ++frameIndex)
				require(outputs[channel][frameIndex]
					== frameIndex + 1
						+ static_cast<dsp56k::TWord>(channel * 100),
					"host output channel mapping or carry order is wrong");
		require(queue.size() == 2,
			"host output queue did not preserve scheduler surplus");
	}
}

int main()
{
	try
	{
		verifyHostAudioInputLookAhead();
		verifyHostAudioInputTimeline();
		verifyHostAudioOutputRouting();
		std::cout << "mdAudioQueueTest: PASS\n";
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "mdAudioQueueTest: " << _error.what() << '\n';
		return 1;
	}
}
