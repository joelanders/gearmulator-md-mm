#pragma once

#include "audiobuffer.h"
#include "functionRef.h"
#include "midiTypes.h"
#include "resampler.h"

#include <memory>	// unique_ptr

namespace synthLib
{
	class ResamplerInOut
	{
	public:
		using TMidiVec = std::vector<SMidiEvent>;
		using TProcessFunc = FunctionRef<void(const TAudioInputs&, const TAudioOutputs&,
			size_t, const TMidiVec&, TMidiVec&)>;

		ResamplerInOut(uint32_t _channelCountIn, uint32_t _channelCountOut);

		void setResamplerMode(Resampler::Mode _mode);
		Resampler::Mode getResamplerMode() const { return m_mode; }
		void setDeviceSamplerate(float _samplerate);
		void setHostSamplerate(float _samplerate);
		void setSamplerates(float _hostSamplerate, float _deviceSamplerate);
		void reconfigure(uint32_t _channelCountIn, uint32_t _channelCountOut,
			float _hostSamplerate, float _deviceSamplerate);
		void reserveMidiEventCapacity(size_t _capacity);
		void prepare(uint32_t _maxHostBlockSize);
		void prepareDeviceSamplerates(const std::vector<float>& _samplerates);
		// Drops every queued MIDI event, including those waiting at a resampled timestamp or in a rate alternative.
		void clearMidi();

		void process(const TAudioInputs& _inputs, TAudioOutputs& _outputs,
			const TMidiVec& _midiIn, TMidiVec& _midiOut, uint32_t _numSamples,
			const TProcessFunc& _processFunc);

		uint32_t getOutputLatency() const { return m_outputLatency; }
		uint32_t getInputLatency() const { return m_inputLatency; }
		uint32_t getInputPaddingSamples() const { return m_inputPadding; }

	private:
		struct TimedMidiEvent
		{
			SMidiEvent event;
			uint64_t sample;
		};

		void recreate();
		void prepareAlternatives();
		void swapStream(ResamplerInOut& _other);
		void clearAudioHistory();
		// Queued events carry absolute positions on the host and device timelines. Before either rate changes, this
		// makes them relative to the current position, converted to the new rates, and restarts both timelines at 0.
		void rebaseQueuedMidi(float _newHostSamplerate, float _newDeviceSamplerate);

		std::vector<float> m_dynamicSamplerates;
		std::vector<std::unique_ptr<ResamplerInOut>> m_alternatives;

		// --- Members are in two groups. swapStream() hands the live stream over to a cached
		// alternative when the device rate changes, so every member below that describes *where the
		// stream currently is* has to move with it, and every member that describes *what this
		// object is for* must stay put. Add a member to the right block.

		// Not swapped: identity, configuration and the live MIDI queues/timelines (rebased instead).
		uint32_t m_channelCountIn;
		uint32_t m_channelCountOut;
		float m_samplerateHost = 0;
		Resampler::Mode m_mode = Resampler::Mode::Legacy;
		uint32_t m_preparedHostBlockSize = 0;
		size_t m_reservedMidiCapacity = 0;

		// Queued input belongs to the live stream, never to a cached rate.
		std::vector<TimedMidiEvent> m_midiIn;
		std::vector<TimedMidiEvent> m_pendingMidiOut;
		uint64_t m_hostSamples = 0;
		uint64_t m_deviceSamples = 0;

		// Swapped by swapStream(): the converters and the audio in flight. Keep this block and
		// swapStream() in step.
		std::unique_ptr<Resampler> m_out = nullptr;
		std::unique_ptr<Resampler> m_in = nullptr;

		float m_samplerateDevice = 0;

		AudioBuffer m_scaledInput;
		AudioBuffer m_input;

		size_t m_scaledInputSize = 0;

		TMidiVec m_processedMidiIn;
		TMidiVec m_midiOut;

		uint32_t m_inputLatency = 0;
		uint32_t m_outputLatency = 0;
		uint32_t m_inputPadding = 0;
	};
}
