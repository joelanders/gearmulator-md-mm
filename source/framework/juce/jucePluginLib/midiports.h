#pragma once

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "juce_audio_devices/juce_audio_devices.h"

namespace synthLib
{
	struct SMidiEvent;
}

namespace baseLib
{
	class ChunkReader;
	class BinaryStream;
}

namespace juce
{
	class String;
}

namespace pluginLib
{
	class Processor;

	class MidiPorts : juce::MidiInputCallback
	{
	public:
		MidiPorts(Processor& _processor);
		MidiPorts(MidiPorts&&) = delete;
		MidiPorts(const MidiPorts&) = delete;

		~MidiPorts() override;

		MidiPorts& operator = (const MidiPorts&) = delete;
		MidiPorts& operator = (MidiPorts&&) = delete;

		auto& getProcessor() const { return m_processor; }

		juce::MidiInput* getMidiInput() const;

		bool setMidiOutput(const juce::String& _out);
		bool setMidiInput(const juce::String& _in);

		juce::String getInputId() const;
		juce::String getOutputId() const;

		void saveChunkData(baseLib::BinaryStream& _binaryStream) const;
		void loadChunkData(baseLib::ChunkReader& _cr);

		void send(juce::MidiMessage&& _message);
		void send(const juce::MidiMessage& _message);

		void send(const synthLib::SMidiEvent& _message)
		{
			return send(toJuceMidiMessage(_message));
		}

		bool trySend(const synthLib::SMidiEvent& _message);

		void close();

		static juce::MidiMessage toJuceMidiMessage(const synthLib::SMidiEvent& _e);

		bool isMidiOutValid() const;

	private:
	    void handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message) override;

		void senderThread();
		bool outputQueueFull() const { return m_midiOutCount == MidiOutCapacity; }
		void pushOutputMessage(juce::MidiMessage&& _message);
		juce::MidiMessage popOutputMessage();
		void clearOutputMessages();

		Processor& m_processor;

		std::unique_ptr<juce::MidiOutput> m_midiOutput{};
		std::unique_ptr<juce::MidiInput> m_midiInput{};
		std::unique_ptr<juce::AudioDeviceManager> m_deviceManager;

		static constexpr size_t MidiOutCapacity = 128;
		std::array<juce::MidiMessage, MidiOutCapacity> m_midiOutMessages;
		size_t m_midiOutRead = 0;
		size_t m_midiOutWrite = 0;
		size_t m_midiOutCount = 0;
		mutable std::mutex m_mutexOutput;
		std::mutex m_mutexOutputConfiguration;
		std::condition_variable m_outputCondition;
		bool m_outputStopping = false;
		std::unique_ptr<std::thread> m_threadOutput;
	};
}
