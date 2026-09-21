#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "mdLib/mdpanel.h"

namespace mdJucePlugin::panelMidi
{
	// Translates MIDI from a controller into front-panel actions. This is separate
	// from the MIDI that reaches the emulated firmware: nothing here is forwarded
	// to the device, it only drives the panel like a person would.
	//
	// A Table says which MIDI source drives which panel control. Each machine has
	// its own controls, so a table is always made for one MachineModel and never
	// binds a control that model does not have.

	struct RawMessage
	{
		uint8_t status = 0;
		uint8_t data1 = 0;
		uint8_t data2 = 0;
	};

	// Only note on/off and controller messages can be bound to a panel control.
	// Pressure (aftertouch), pitch bend, program change and system messages are
	// dropped before they are queued, so pads that keep sending pressure while
	// held cannot clutter the "last received" line or fill the queue.
	constexpr bool isBindableMessage(const uint8_t _status)
	{
		const auto type = _status & 0xf0;
		return _status >= 0x80 && _status < 0xf0 && (type == 0x80 || type == 0x90 || type == 0xb0);
	}

	enum class EncoderMode : uint8_t
	{
		Absolute,				// knob/pot: the change since the last value turns the encoder
		RelativeOffset,			// 64 = still, 65 = +1, 63 = -1
		RelativeTwosComplement	// 1..63 = +n, 127..65 = -n
	};

	constexpr uint8_t g_encoderModeCount = 3;

	struct Source
	{
		enum class Kind : uint8_t
		{
			None,
			Note,
			Controller
		};

		Kind kind = Kind::None;
		uint8_t number = 0;

		constexpr bool valid() const { return kind != Kind::None; }
		constexpr bool operator==(const Source& _other) const
		{
			return kind == _other.kind && (kind == Kind::None || number == _other.number);
		}
		constexpr bool operator!=(const Source& _other) const { return !(*this == _other); }
	};

	constexpr size_t g_encoderCount = static_cast<size_t>(md::PanelEncoder::SoundSelection) + 1;
	// The data entry encoders A-H can also be pressed; Level and Sound selection cannot.
	constexpr size_t g_pushCount = static_cast<size_t>(md::PanelEncoder::DataEntryH) + 1;
	constexpr size_t g_controlCount = static_cast<size_t>(md::PanelControl::ClassicExtended) + 1;

	struct EncoderBinding
	{
		Source source;	// controllers only
		EncoderMode mode = EncoderMode::Absolute;
	};

	struct Table
	{
		uint8_t channel = 0;	// 0 = omni, 1..16 = only that channel
		std::array<EncoderBinding, g_encoderCount> encoders;
		std::array<Source, g_controlCount> buttons;	// indexed by md::PanelControl
		std::array<Source, g_pushCount> pushes;		// encoder push switches, indexed by md::PanelEncoder

		bool operator==(const Table& _other) const;
		bool operator!=(const Table& _other) const { return !(*this == _other); }
	};

	// Whether the model has that control. The panel packet is the authority the
	// editor itself uses to decide whether a control can be operated.
	bool isAvailable(md::MachineModel _model, md::PanelEncoder _encoder);
	bool isAvailable(md::MachineModel _model, md::PanelControl _control);
	bool isPushAvailable(md::MachineModel _model, md::PanelEncoder _encoder);

	// Fixed factory map:
	//   CC 16-25  encoders A-H, Level, Sound selection
	//   CC 65-80  trig keys 1-16
	//   CC 26-56  the other buttons, in the order of g_buttonControllers
	//   CC 57-64  push switches of encoders A-H
	// Controls the model does not have are left unbound.
	Table makeDefaultTable(md::MachineModel _model);

	// Plain-text form, one line per binding, meant to be readable and editable:
	//   gearmulator-panel-midi 2
	//   channel 0
	//   encoder DataEntryA cc 16 absolute
	//   push DataEntryA cc 57
	//   button Trigger1 note 36
	// Version 1 files predate the push switches; they load with the default pushes.
	std::string toText(const Table& _table);

	// Reads a table for the model. Bindings for unknown names or controls the
	// model does not have are skipped. False if the text is not a table at all.
	bool fromText(const std::string& _text, md::MachineModel _model, Table& _table);

	std::string describe(const Source& _source);	// "CC 16", "Note 36" or "-"
	std::string describe(const RawMessage& _message);	// "CC 17 = 65 (ch 1)"
	const char* describe(EncoderMode _mode);

	constexpr uint8_t g_firstEncoderController = 16;
	constexpr uint8_t g_firstTriggerController = 65;
	constexpr uint8_t g_firstButtonController = 26;
	constexpr uint8_t g_firstPushController = 57;

	struct Action
	{
		enum class Kind : uint8_t
		{
			EncoderSteps,
			ButtonDown,
			ButtonUp,
			EncoderPushDown,
			EncoderPushUp
		};

		Kind kind = Kind::EncoderSteps;
		md::PanelEncoder encoder = md::PanelEncoder::DataEntryA;
		md::PanelControl control = md::PanelControl::Trigger1;
		int steps = 0;	// signed detents, EncoderSteps only (EncoderPushDown/Up use `encoder`)

		constexpr bool operator==(const Action& _other) const
		{
			return kind == _other.kind && encoder == _other.encoder
				&& control == _other.control && steps == _other.steps;
		}
	};

	bool passesChannel(const Table& _table, uint8_t _status);

	// The machine listens to a block of consecutive MIDI channels starting at its
	// base channel (a Global setting of the machine): four on the Machinedrum, six
	// on the Monomachine. On those channels a controller change is taken as a
	// parameter value, so a controller that also reaches the machine's own MIDI
	// input changes its parameters as well as driving the panel.
	constexpr uint8_t machineChannelCount(const md::MachineModel _model)
	{
		return _model == md::MachineModel::Monomachine ? 6 : 4;
	}

	// Warning text when the panel channel filter (0 = omni, 1-16) includes a
	// channel the machine listens on; nothing otherwise. baseChannel is zero-based
	// as the machine reports it. A value above 15 means it is not known yet, which
	// gives no warning rather than a guess.
	std::optional<std::string> channelOverlapWarning(md::MachineModel _model, uint8_t _panelChannel,
		uint8_t _baseChannel);

	class Map
	{
	public:
		Map() { setTable(Table()); }

		// Replaces the bindings. Also forgets absolute-mode history, so the next
		// absolute value only re-anchors and the encoders cannot jump.
		void setTable(const Table& _table);
		const Table& getTable() const { return m_table; }

		void reset();

		// One raw channel-voice message. Returns nothing for messages that are
		// filtered out or not bound. Not thread safe: call from one thread.
		std::optional<Action> translate(const RawMessage& _message);
		std::optional<Action> translate(uint8_t _status, uint8_t _data1, uint8_t _data2)
		{
			return translate(RawMessage{ _status, _data1, _data2 });
		}

	private:
		struct Target
		{
			enum class Kind : uint8_t { None, Encoder, Button, Push };
			Kind kind = Kind::None;
			uint8_t index = 0;
		};

		Table m_table;
		std::array<Target, 128> m_notes;
		std::array<Target, 128> m_controllers;
		std::array<int16_t, g_encoderCount> m_lastAbsolute;
	};
}
