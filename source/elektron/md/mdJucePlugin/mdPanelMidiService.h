#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "mdPanelMidiController.h"
#include "mdPanelMidiDriver.h"

#include "juce_events/juce_events.h"

namespace mdJucePlugin::panelMidi
{
	// Panel MIDI for one plugin instance. The controller is routed to the
	// plugin's normal MIDI input, like any MIDI learn: a message that is bound to
	// a panel control (or any message while learning) drives the panel and is
	// held back from the emulated machine; everything else reaches the machine
	// as before.
	//
	// The bindings and the Driver live here rather than in the editor, so the
	// panel keeps working while the plugin window is closed.
	//
	// Parameter MIDI Learn comes first: a message it has mapped, or any message
	// while it is learning, is left to it (see the processor's consumeMidiInput),
	// and panel learn does not take such a message.
	//
	// intercept() may be called from the audio thread and from MIDI input
	// threads: it only reads atomics and queues the message. Everything else is
	// message thread only, except the project state accessors and requestReleaseAll().
	class Service final : private juce::Timer
	{
	public:
		// filePath holds the last-used bindings, which new instances start from;
		// empty keeps them in memory only. Instances start with nothing bound.
		Service(md::MachineModel _model, std::string _filePath, md::PanelRowMixer& _rows,
			Driver::SendFunc _send);
		~Service() override;

		Service(const Service&) = delete;
		Service& operator = (const Service&) = delete;

		Controller& getController() { return m_controller; }
		Driver& getDriver() { return m_driver; }

		// True when the message belongs to the panel and must not reach the
		// machine. reservedByMidiLearn: parameter MIDI Learn has mapped it, so it
		// is never taken. All Notes Off (CC 123) on the panel channel lets go of
		// everything held from MIDI and still reaches the machine. Any thread.
		bool intercept(uint8_t _status, uint8_t _data1, uint8_t _data2, bool _reservedByMidiLearn = false);

		// Lets go of everything held from MIDI on the next service(), e.g. after a
		// project was loaded. Any thread.
		void requestReleaseAll() { m_releaseAllRequested.store(true, std::memory_order_relaxed); }

		// The bindings as stored in a project. Any thread; a loaded table is
		// applied on the message thread shortly after.
		std::string getProjectState() const;
		void setProjectState(std::string _text);

		// Applies a loaded project, hands queued messages to the controller and
		// lets the driver send what is due. The timer calls this; tests may call
		// it directly with their own clock.
		void service();
		void service(double _nowMs);

	private:
		struct Pending;
		void timerCallback() override { service(); }
		void publish();
		bool queue(const Pending& _pending);

		static constexpr size_t g_maxPending = 1024;

		struct Pending
		{
			RawMessage message;
			bool reserved = false;	// mapped by parameter MIDI Learn: shown, not learned
		};

		const md::MachineModel m_model;
		Controller m_controller;
		Driver m_driver;
		double m_nowMs = 0.0;

		// Read by intercept(), written by publish() on the message thread.
		std::atomic<uint8_t> m_channel{0};
		std::atomic<bool> m_learning{false};
		std::atomic<bool> m_releaseAllRequested{false};
		std::array<std::atomic<bool>, 128> m_notes{};
		std::array<std::atomic<bool>, 128> m_controllers{};

		std::mutex m_pendingMutex;
		std::vector<Pending> m_pending;
		std::vector<Pending> m_draining;

		mutable std::mutex m_stateMutex;
		std::string m_projectState;
		std::optional<std::string> m_loadedState;
	};
}
