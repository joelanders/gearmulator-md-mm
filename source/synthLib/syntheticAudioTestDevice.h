#pragma once

#include "device.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace synthLib::test
{
	class SyntheticAudioDevice final : public Device
	{
	public:
		SyntheticAudioDevice(const uint32_t _inputs, const uint32_t _outputs,
			const uint32_t _inputLatency, const bool _silent = false,
			const float _outputLevel = 0.01f)
			: Device({}), m_inputs(_inputs), m_outputs(_outputs),
			m_inputLatency(_inputLatency), m_silent(_silent),
			m_outputLevel(_outputLevel)
		{
		}

		float getSamplerate() const override { return 44100.0f; }
		bool isValid() const override { return m_valid; }
#if SYNTHLIB_DEMO_MODE == 0
		bool getState(std::vector<uint8_t>&, StateType) override { return false; }
		bool setState(const std::vector<uint8_t>&, StateType) override { return false; }
#endif
		uint32_t getChannelCountIn() override { return m_inputs; }
		uint32_t getChannelCountOut() override { return m_outputs; }
		uint32_t getInternalLatencyInputToOutput() const override
		{
			return m_inputLatency;
		}
		bool setDspClockPercent(uint32_t) override { return false; }
		uint32_t getDspClockPercent() const override { return 100; }
		uint64_t getDspClockHz() const override { return 100000000; }
		bool receivedEveryOutput() const { return m_receivedEveryOutput; }
		bool receivedDistinctOutputs() const { return m_receivedDistinctOutputs; }
		bool receivedEveryInput() const { return m_receivedEveryInput; }
		float getInputPeak(const size_t _channel) const { return m_inputPeaks[_channel]; }
		void invalidate() { m_valid = false; }

	private:
		void readMidiOut(std::vector<SMidiEvent>&) override {}
		bool sendMidi(const SMidiEvent&, std::vector<SMidiEvent>&) override
		{
			return true;
		}
		void processAudio(const TAudioInputs& _inputs,
			const TAudioOutputs& _outputs, const size_t _samples) override
		{
			for(uint32_t channel = 0; channel < m_inputs; ++channel)
			{
				if(!_inputs[channel])
				{
					m_receivedEveryInput = false;
					continue;
				}
				for(size_t sample = 0; sample < _samples; ++sample)
					m_inputPeaks[channel] = std::max(m_inputPeaks[channel],
						std::abs(_inputs[channel][sample]));
			}
			for(uint32_t channel = 0; channel < m_outputs; ++channel)
			{
				if(!_outputs[channel])
				{
					m_receivedEveryOutput = false;
					continue;
				}
				for(uint32_t previous = 0; previous < channel; ++previous)
					m_receivedDistinctOutputs = m_receivedDistinctOutputs
						&& _outputs[channel] != _outputs[previous];
				std::fill_n(_outputs[channel], _samples, m_silent ? 0.0f
					: m_outputLevel * static_cast<float>(channel + 1));
			}
		}

		uint32_t m_inputs;
		uint32_t m_outputs;
		uint32_t m_inputLatency;
		bool m_silent;
		float m_outputLevel;
		bool m_receivedEveryOutput = true;
		bool m_receivedDistinctOutputs = true;
		bool m_receivedEveryInput = true;
		std::array<float, 4> m_inputPeaks{};
		bool m_valid = true;
	};
}
