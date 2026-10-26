#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mdPanelMidiMap.h"

#include "juce_audio_devices/juce_audio_devices.h"
#include "juce_events/juce_events.h"

namespace mdJucePlugin::panelMidi
{
	// Config key: create the panel MIDI virtual port (default on). Set it to
	// false to stop an instance from publishing a port.
	constexpr auto g_configKeyVirtualPort = "panelMidiVirtualPort";

	// A virtual MIDI input for controlling the emulator's front panel. It is not
	// connected to the emulated device. The MIDI thread only queues the raw
	// channel messages; they are handed to the handler on the message thread,
	// which is the only thread that may touch the editor.
	//
	// Several instances can live in one process (plugins in a DAW). The first
	// keeps the plain port name and later ones get " 2", " 3", ... so they can be
	// told apart; a number is reused once its instance is gone.
	class Input final : private juce::MidiInputCallback, private juce::AsyncUpdater
	{
	public:
		using Handler = std::function<void(const RawMessage&)>;

		Input(const std::string& _portName, Handler _handler);
		~Input() override;

		Input(const Input&) = delete;
		Input& operator = (const Input&) = delete;

		// False when the port could not be created, e.g. on Windows where JUCE
		// has no virtual MIDI ports.
		bool isOpen() const { return m_input != nullptr; }
		const std::string& getPortName() const { return m_portName; }

	private:
		void handleIncomingMidiMessage(juce::MidiInput* _source, const juce::MidiMessage& _message) override;
		void handleAsyncUpdate() override;

		static constexpr size_t g_maxPending = 1024;

		std::string m_baseName;
		int m_instanceNumber = 0;
		std::string m_portName;
		Handler m_handler;
		std::mutex m_mutex;
		std::vector<RawMessage> m_pending;
		std::unique_ptr<juce::MidiInput> m_input;
	};
}
