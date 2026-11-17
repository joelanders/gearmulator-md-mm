#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>

#include "mdPanelMidiMap.h"

namespace mdJucePlugin::panelMidi
{
	// Operates the emulated panel for panel MIDI, without the editor. Presses and
	// releases go into the shared row mixer as the Midi source, so the editor's
	// mouse handling and focus loss never let go of a key held from a controller.
	//
	// The panel is a key matrix the firmware scans, so a key must stay in one
	// state long enough to be seen. A pad tap can send note on and off within a
	// millisecond; each key's edges are therefore spaced at least
	// g_minEdgeIntervalMs apart, later edges waiting their turn in order.
	//
	// Message thread only, like the mixer.
	class Driver
	{
	public:
		// Sends one panel packet to the machine; false if it was not accepted.
		using SendFunc = std::function<bool(uint8_t _command, uint8_t _argument)>;
		// Told about every encoder turn so an open editor can turn its knob too.
		using EncoderTurnedFunc = std::function<void(md::PanelEncoder _encoder, int _steps)>;

		static constexpr double g_minEdgeIntervalMs = 30.0;
		// The most single detents one message sends, as for one mouse move.
		static constexpr int g_encoderBurstCap = 8;

		Driver(md::MachineModel _model, md::PanelRowMixer& _rows, SendFunc _send);

		void setEncoderTurnedHandler(EncoderTurnedFunc _handler) { m_encoderTurned = std::move(_handler); }

		void apply(const Action& _action, double _nowMs);
		// Sends edges whose turn has come. Call often (the service timer does).
		void service(double _nowMs);
		// Lets go of everything held from MIDI and drops edges still waiting.
		void releaseAll(double _nowMs);

		bool isHeld(md::PanelControl _control) const;
		bool isPushHeld(md::PanelEncoder _encoder) const;
		bool hasPendingEdges() const;

	private:
		struct Key
		{
			std::optional<md::PanelPacket> packet;	// none: the model lacks this control
			bool down = false;
			bool everSent = false;
			double lastEdgeMs = 0.0;
			std::deque<bool> pending;	// press = true
		};

		static constexpr size_t g_keyCount = g_controlCount + g_pushCount;
		static constexpr size_t g_maxPendingEdges = 16;

		void queueEdge(size_t _key, bool _down, double _nowMs);
		void sendEdge(Key& _key, bool _down, double _nowMs);

		const md::MachineModel m_model;
		md::PanelRowMixer::Port m_rows;
		SendFunc m_send;
		EncoderTurnedFunc m_encoderTurned;
		std::array<Key, g_keyCount> m_keys;
	};
}
