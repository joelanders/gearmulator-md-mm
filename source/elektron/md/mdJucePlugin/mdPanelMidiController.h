#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mdPanelMidiMap.h"

namespace mdJucePlugin::panelMidi
{
	// Owns the panel MIDI bindings for one machine: applies incoming messages to
	// the panel, lets the settings page edit the bindings, learns a binding from
	// the next controller message, and keeps the table in a text file.
	//
	// Everything here runs on the message thread; the MIDI thread only hands raw
	// messages over (see Input). That keeps the table free of locks even while it
	// is edited.
	class Controller
	{
	public:
		using ActionHandler = std::function<void(const Action&)>;

		struct LearnTarget
		{
			enum class Kind : uint8_t { None, Encoder, Button, EncoderPush };

			Kind kind = Kind::None;
			md::PanelEncoder encoder = md::PanelEncoder::DataEntryA;
			md::PanelControl control = md::PanelControl::Trigger1;

			bool active() const { return kind != Kind::None; }
			bool is(const md::PanelEncoder _encoder) const { return kind == Kind::Encoder && encoder == _encoder; }
			bool is(const md::PanelControl _control) const { return kind == Kind::Button && control == _control; }
			bool isPush(const md::PanelEncoder _encoder) const { return kind == Kind::EncoderPush && encoder == _encoder; }
		};

		// filePath may be empty to keep the table in memory only.
		Controller(md::MachineModel _model, std::string _filePath, ActionHandler _handler);

		md::MachineModel getModel() const { return m_model; }
		const Table& getTable() const { return m_map.getTable(); }

		void handleMessage(const RawMessage& _message);

		void setChannel(uint8_t _channel);
		void setEncoderMode(md::PanelEncoder _encoder, EncoderMode _mode);
		void clear(md::PanelEncoder _encoder);
		void clear(md::PanelControl _control);
		void clearPush(md::PanelEncoder _encoder);
		void resetToDefault();

		// While learning, the next note or controller message becomes the binding
		// and nothing is sent to the panel. A source that another control already
		// uses moves to the new control.
		void beginLearn(md::PanelEncoder _encoder);
		void beginLearn(md::PanelControl _control);
		void beginLearnPush(md::PanelEncoder _encoder);	// the encoder's push switch, not its turning
		void cancelLearn();
		const LearnTarget& getLearnTarget() const { return m_learn; }

		// Learns a whole list, one control after another: each accepted message
		// binds the current control and moves on to the next, so 16 pads can be
		// mapped with 16 presses. A source already learned during this run is
		// ignored, so a repeated press, a release (a CC going back to 0) or a knob
		// that keeps sending cannot take the previous control's binding away.
		// Controls this machine does not have are left out of the list.
		void beginLearnSequence(const std::vector<LearnTarget>& _targets);
		bool isLearningSequence() const { return !m_sequence.empty(); }
		size_t getSequenceIndex() const { return m_sequenceIndex; }
		size_t getSequenceSize() const { return m_sequence.size(); }

		// Counters the settings page polls to know when to redraw.
		uint32_t getRevision() const { return m_revision; }
		uint32_t getMessageSerial() const { return m_messageSerial; }
		const RawMessage& getLastMessage() const { return m_lastMessage; }
		// True when the last message was a channel message that the channel filter
		// dropped, so the page can say why nothing happened.
		bool wasLastMessageFiltered() const { return m_lastMessageFiltered; }

		bool save() const;

	private:
		void load();
		void replaceTable(const Table& _table);
		bool tryLearn(const RawMessage& _message);
		void endSequence();

		const md::MachineModel m_model;
		const std::string m_filePath;
		ActionHandler m_handler;
		Map m_map;
		LearnTarget m_learn;
		RawMessage m_lastMessage;
		bool m_lastMessageFiltered = false;
		std::vector<LearnTarget> m_sequence;
		size_t m_sequenceIndex = 0;
		std::vector<Source> m_sequenceLearned;
		uint32_t m_revision = 0;
		uint32_t m_messageSerial = 0;
	};
}
