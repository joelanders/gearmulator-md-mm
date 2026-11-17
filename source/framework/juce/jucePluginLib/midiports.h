#pragma once

#include <array>
#include <atomic>
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

		// A port whose device is away stays selected and opens once the device is back. When the system's MIDI devices
		// change, the ports whose devices went away, came back or moved in the lists reopen by themselves: a device
		// plugged in again, or back after a USB hiccup, has new handles, the old ones send and get nothing
		bool setMidiOutput(const juce::String& _out);
		bool setMidiInput(const juce::String& _in);

		// the ports that are open
		juce::String getInputId() const;
		juce::String getOutputId() const;

		void saveChunkData(baseLib::BinaryStream& _binaryStream) const;
		void loadChunkData(baseLib::ChunkReader& _cr);

		// Fork: non-realtime senders wait for room in the fixed-capacity output queue rather than drop a message
		void send(juce::MidiMessage&& _message);
		void send(const juce::MidiMessage& _message);

		void send(const synthLib::SMidiEvent& _message)
		{
			return send(toJuceMidiMessage(_message));
		}

		// Fork: realtime producers only try the queue lock and never wait. False if the message could not be queued
		bool trySend(const synthLib::SMidiEvent& _message);

		void close();

		static juce::MidiMessage toJuceMidiMessage(const synthLib::SMidiEvent& _e);

		bool isMidiOutValid() const;

	private:
	    void handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message) override;

		// setMidiInput() and setMidiOutput() without changing the selection
		bool openMidiInput(const juce::String& _in);
		bool openMidiOutput(const juce::String& _out);

		// The system's MIDI devices changed, for all instances. Closes the ports whose devices are not where they were
		// in the lists, then opens the selected ports that are closed
		static void listenToDeviceList();
		static void onDevicesChanged();
		void closeMovedPorts(const juce::Array<juce::MidiDeviceInfo>& _inputs, const juce::Array<juce::MidiDeviceInfo>& _outputs);
		void openClosedPorts();

		Processor& m_processor;

		// the ports the user or a state selected, open or not: their device may be away for a while
		juce::String m_selectedInput;
		juce::String m_selectedOutput;
		// selecting and reopening happen on the message thread and wherever a host loads a state
		mutable std::mutex m_mutexSelection;
		// where the open ports' devices were in the system's lists: one that is listed elsewhere now went away and came
		// back, plugged in again or after a USB hiccup
		int m_inputIndex = -1;
		int m_outputIndex = -1;

		// Fork: the output queue and its sender thread, see MidiOutputDispatcher
		MidiOutputDispatcher m_midiOutput;
		std::unique_ptr<juce::MidiInput> m_midiInput{};
	};
}
