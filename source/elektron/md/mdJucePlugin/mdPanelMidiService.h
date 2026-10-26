#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mdPanelMidiController.h"

#include "juce_events/juce_events.h"

namespace mdJucePlugin::panelMidi
{
	// Panel MIDI for one plugin instance. The controller is routed to the
	// plugin's normal MIDI input, like any MIDI learn: a message that is bound to
	// a panel control (or any message while learning) drives the panel and is
	// held back from the emulated machine; everything else reaches the machine
	// as before.
	//
	// The bindings live here rather than in the editor so that they keep
	// filtering while the editor is closed. Panel actions need the editor, so
	// they are dropped while none is attached.
	//
	// intercept() may be called from the audio thread and from MIDI input
	// threads: it only reads atomics and queues the message. Everything else is
	// message thread only, except the project state accessors.
	class Service final : private juce::Timer
	{
	public:
		// filePath holds the last-used bindings, which new instances start from;
		// empty keeps them in memory only. Instances start with nothing bound.
		Service(md::MachineModel _model, std::string _filePath);
		~Service() override;

		Service(const Service&) = delete;
		Service& operator = (const Service&) = delete;

		Controller& getController() { return m_controller; }
		void setActionHandler(Controller::ActionHandler _handler);

		// True when the message belongs to the panel and must not reach the
		// machine. Any thread.
		bool intercept(uint8_t _status, uint8_t _data1, uint8_t _data2);

		// The bindings as stored in a project. Any thread; a loaded table is
		// applied on the message thread shortly after.
		std::string getProjectState() const;
		void setProjectState(std::string _text);

		// Applies a loaded project and hands queued messages to the controller.
		// The timer calls this; tests may call it directly.
		void service();

	private:
		void timerCallback() override { service(); }
		void publish();

		static constexpr size_t g_maxPending = 1024;

		const md::MachineModel m_model;
		Controller m_controller;

		// Read by intercept(), written by publish() on the message thread.
		std::atomic<uint8_t> m_channel{0};
		std::atomic<bool> m_learning{false};
		std::array<std::atomic<bool>, 128> m_notes{};
		std::array<std::atomic<bool>, 128> m_controllers{};

		std::mutex m_pendingMutex;
		std::vector<RawMessage> m_pending;
		std::vector<RawMessage> m_draining;

		mutable std::mutex m_stateMutex;
		std::string m_projectState;
		std::optional<std::string> m_loadedState;
	};
}
