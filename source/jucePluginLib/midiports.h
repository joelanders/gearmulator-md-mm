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
	class MidiOutputSink
	{
	public:
		virtual ~MidiOutputSink() = default;
		virtual juce::String getIdentifier() const = 0;
		virtual void start() = 0;
		virtual void stop() = 0;
		virtual bool isRunning() const = 0;
		virtual void sendMessageNow(const juce::MidiMessage& _message) = 0;
	};

	// Fixed-capacity asynchronous output shared by physical MIDI and its controlled
	// tests. Realtime producers only try the queue lock and never wait; lifecycle
	// changes stop and join the sole consumer before replacing its sink.
	class MidiOutputDispatcher
	{
	public:
		static constexpr size_t Capacity = 128;

		MidiOutputDispatcher() = default;
		~MidiOutputDispatcher();
		MidiOutputDispatcher(const MidiOutputDispatcher&) = delete;
		MidiOutputDispatcher(MidiOutputDispatcher&&) = delete;
		MidiOutputDispatcher& operator=(const MidiOutputDispatcher&) = delete;
		MidiOutputDispatcher& operator=(MidiOutputDispatcher&&) = delete;

		void send(juce::MidiMessage&& _message);
		void send(const juce::MidiMessage& _message);
		bool trySend(juce::MidiMessage&& _message);
		bool setOutput(std::unique_ptr<MidiOutputSink> _output);
		void close();
		bool isValid() const;
		juce::String getOutputId() const;

	private:
		void senderThread();
		bool outputQueueFull() const { return m_count == Capacity; }
		void push(juce::MidiMessage&& _message);
		juce::MidiMessage pop();
		void clear();

		std::unique_ptr<MidiOutputSink> m_output;
		std::array<juce::MidiMessage, Capacity> m_messages;
		size_t m_read = 0;
		size_t m_write = 0;
		size_t m_count = 0;
		mutable std::mutex m_mutex;
		std::mutex m_configurationMutex;
		std::condition_variable m_condition;
		bool m_stopping = false;
		std::unique_ptr<std::thread> m_thread;
	};

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

		Processor& m_processor;

		MidiOutputDispatcher m_midiOutput;
		std::unique_ptr<juce::MidiInput> m_midiInput{};
		std::unique_ptr<juce::AudioDeviceManager> m_deviceManager;
	};
}
