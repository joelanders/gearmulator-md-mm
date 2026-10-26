#include "mdPanelMidiMap.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <sstream>

namespace mdJucePlugin::panelMidi
{
	namespace
	{
		constexpr uint8_t g_statusNoteOff = 0x80;
		constexpr uint8_t g_statusNoteOn = 0x90;
		constexpr uint8_t g_statusController = 0xb0;

		constexpr auto g_textHeader = "gearmulator-panel-midi";
		constexpr int g_textVersion = 2;

		// Controls addressed by CC in the factory map, starting at g_firstButtonController.
		constexpr md::PanelControl g_buttonControllers[] =
		{
			md::PanelControl::Track1, md::PanelControl::Track2, md::PanelControl::Track3,
			md::PanelControl::Track4, md::PanelControl::Track5, md::PanelControl::Track6,
			md::PanelControl::Function, md::PanelControl::Kit,
			md::PanelControl::Enter, md::PanelControl::Exit,
			md::PanelControl::Up, md::PanelControl::Down,
			md::PanelControl::Left, md::PanelControl::Right,
			md::PanelControl::Play, md::PanelControl::Stop, md::PanelControl::Record,
			md::PanelControl::Tempo, md::PanelControl::SynthesisEffectsRouting,
			md::PanelControl::PatternSong, md::PanelControl::Scale,
			md::PanelControl::TrigSelect, md::PanelControl::SongEnable,
			md::PanelControl::DataPageForward, md::PanelControl::DataPageBackward,
			md::PanelControl::BankGroup,
			md::PanelControl::BankA, md::PanelControl::BankB,
			md::PanelControl::BankC, md::PanelControl::BankD,
			md::PanelControl::ClassicExtended,
		};

		constexpr bool isTrigger(const md::PanelControl _control)
		{
			return _control >= md::PanelControl::Trigger1 && _control <= md::PanelControl::Trigger16;
		}

		Action encoderAction(const size_t _index, const int _steps)
		{
			Action a;
			a.kind = Action::Kind::EncoderSteps;
			a.encoder = static_cast<md::PanelEncoder>(_index);
			a.steps = _steps;
			return a;
		}

		Action pushAction(const size_t _index, const bool _down)
		{
			Action a;
			a.kind = _down ? Action::Kind::EncoderPushDown : Action::Kind::EncoderPushUp;
			a.encoder = static_cast<md::PanelEncoder>(_index);
			return a;
		}

		Action buttonAction(const size_t _index, const bool _down)
		{
			Action a;
			a.kind = _down ? Action::Kind::ButtonDown : Action::Kind::ButtonUp;
			a.control = static_cast<md::PanelControl>(_index);
			return a;
		}

		const char* modeText(const EncoderMode _mode)
		{
			switch(_mode)
			{
			case EncoderMode::Absolute: return "absolute";
			case EncoderMode::RelativeOffset: return "relative-offset";
			case EncoderMode::RelativeTwosComplement: return "relative-twos";
			}
			return "absolute";
		}

		std::optional<EncoderMode> modeFromText(const std::string& _text)
		{
			for(uint8_t i = 0; i < g_encoderModeCount; ++i)
				if(_text == modeText(static_cast<EncoderMode>(i)))
					return static_cast<EncoderMode>(i);
			return std::nullopt;
		}

		std::optional<size_t> encoderFromName(const std::string& _name)
		{
			for(size_t i = 0; i < g_encoderCount; ++i)
				if(_name == md::panelEncoderName(static_cast<md::PanelEncoder>(i)))
					return i;
			return std::nullopt;
		}

		std::optional<size_t> controlFromName(const std::string& _name)
		{
			for(size_t i = 0; i < g_controlCount; ++i)
				if(_name == md::panelControlName(static_cast<md::PanelControl>(i)))
					return i;
			return std::nullopt;
		}

		std::optional<int> parseNumber(const std::string& _text, const int _max)
		{
			if(_text.empty() || _text.size() > 3)
				return std::nullopt;
			for(const auto c : _text)
				if(c < '0' || c > '9')
					return std::nullopt;
			const auto value = std::atoi(_text.c_str());
			if(value > _max)
				return std::nullopt;
			return value;
		}
	}

	bool Table::operator==(const Table& _other) const
	{
		if(channel != _other.channel || buttons != _other.buttons || pushes != _other.pushes)
			return false;
		for(size_t i = 0; i < encoders.size(); ++i)
			if(encoders[i].source != _other.encoders[i].source || encoders[i].mode != _other.encoders[i].mode)
				return false;
		return true;
	}

	bool isAvailable(const md::MachineModel _model, const md::PanelEncoder _encoder)
	{
		return md::panelEncoderCommand(_model, _encoder).has_value();
	}

	bool isAvailable(const md::MachineModel _model, const md::PanelControl _control)
	{
		return md::panelPacket(_model, _control).has_value();
	}

	bool isPushAvailable(const md::MachineModel _model, const md::PanelEncoder _encoder)
	{
		return md::panelEncoderPressPacket(_model, _encoder).has_value();
	}

	namespace
	{
		void addDefaultPushes(const md::MachineModel _model, Table& _table)
		{
			for(size_t i = 0; i < g_pushCount; ++i)
			{
				if(isPushAvailable(_model, static_cast<md::PanelEncoder>(i)))
					_table.pushes[i] = { Source::Kind::Controller, static_cast<uint8_t>(g_firstPushController + i) };
			}
		}
	}

	Table makeDefaultTable(const md::MachineModel _model)
	{
		Table table;
		addDefaultPushes(_model, table);

		for(size_t i = 0; i < g_encoderCount; ++i)
		{
			if(isAvailable(_model, static_cast<md::PanelEncoder>(i)))
				table.encoders[i].source = { Source::Kind::Controller, static_cast<uint8_t>(g_firstEncoderController + i) };
		}

		for(size_t i = 0; i < g_controlCount; ++i)
		{
			const auto control = static_cast<md::PanelControl>(i);
			if(!isAvailable(_model, control))
				continue;
			if(isTrigger(control))
			{
				const auto index = static_cast<uint8_t>(i - static_cast<size_t>(md::PanelControl::Trigger1));
				table.buttons[i] = { Source::Kind::Controller, static_cast<uint8_t>(g_firstTriggerController + index) };
			}
		}

		for(size_t i = 0; i < std::size(g_buttonControllers); ++i)
		{
			const auto control = g_buttonControllers[i];
			if(isAvailable(_model, control))
				table.buttons[static_cast<size_t>(control)] = { Source::Kind::Controller, static_cast<uint8_t>(g_firstButtonController + i) };
		}

		return table;
	}

	std::string toText(const Table& _table)
	{
		std::ostringstream out;
		out << g_textHeader << ' ' << g_textVersion << '\n';
		out << "channel " << static_cast<int>(_table.channel) << '\n';

		for(size_t i = 0; i < g_encoderCount; ++i)
		{
			const auto& b = _table.encoders[i];
			if(b.source.kind != Source::Kind::Controller)
				continue;
			out << "encoder " << md::panelEncoderName(static_cast<md::PanelEncoder>(i))
				<< " cc " << static_cast<int>(b.source.number) << ' ' << modeText(b.mode) << '\n';
		}

		for(size_t i = 0; i < g_pushCount; ++i)
		{
			const auto& s = _table.pushes[i];
			if(!s.valid())
				continue;
			out << "push " << md::panelEncoderName(static_cast<md::PanelEncoder>(i))
				<< (s.kind == Source::Kind::Note ? " note " : " cc ") << static_cast<int>(s.number) << '\n';
		}

		for(size_t i = 0; i < g_controlCount; ++i)
		{
			const auto& s = _table.buttons[i];
			if(!s.valid())
				continue;
			out << "button " << md::panelControlName(static_cast<md::PanelControl>(i))
				<< (s.kind == Source::Kind::Note ? " note " : " cc ") << static_cast<int>(s.number) << '\n';
		}
		return out.str();
	}

	bool fromText(const std::string& _text, const md::MachineModel _model, Table& _table)
	{
		std::istringstream in(_text);
		std::string line;

		if(!std::getline(in, line))
			return false;
		int version = 0;
		{
			std::istringstream header(line);
			std::string name;
			if(!(header >> name >> version) || name != g_textHeader || version < 1 || version > g_textVersion)
				return false;
		}

		Table table;
		// Version 1 had no push switches: keep them working rather than unbound.
		if(version < 2)
			addDefaultPushes(_model, table);

		while(std::getline(in, line))
		{
			std::istringstream words(line);
			std::string kind;
			if(!(words >> kind))
				continue;

			if(kind == "channel")
			{
				std::string value;
				if(words >> value)
					if(const auto channel = parseNumber(value, 16))
						table.channel = static_cast<uint8_t>(*channel);
				continue;
			}

			std::string name, type, number;
			if(!(words >> name >> type >> number))
				continue;
			const auto n = parseNumber(number, 127);
			if(!n)
				continue;

			if(kind == "encoder")
			{
				std::string mode;
				words >> mode;
				const auto index = encoderFromName(name);
				const auto parsedMode = modeFromText(mode);
				if(!index || type != "cc" || !parsedMode
					|| !isAvailable(_model, static_cast<md::PanelEncoder>(*index)))
					continue;
				table.encoders[*index] = { { Source::Kind::Controller, static_cast<uint8_t>(*n) }, *parsedMode };
			}
			else if(kind == "push")
			{
				const auto index = encoderFromName(name);
				if(!index || *index >= g_pushCount || (type != "note" && type != "cc")
					|| !isPushAvailable(_model, static_cast<md::PanelEncoder>(*index)))
					continue;
				table.pushes[*index] = { type == "note" ? Source::Kind::Note : Source::Kind::Controller, static_cast<uint8_t>(*n) };
			}
			else if(kind == "button")
			{
				const auto index = controlFromName(name);
				if(!index || (type != "note" && type != "cc")
					|| !isAvailable(_model, static_cast<md::PanelControl>(*index)))
					continue;
				table.buttons[*index] = { type == "note" ? Source::Kind::Note : Source::Kind::Controller, static_cast<uint8_t>(*n) };
			}
		}

		_table = table;
		return true;
	}

	std::string describe(const Source& _source)
	{
		switch(_source.kind)
		{
		case Source::Kind::Note: return "Note " + std::to_string(_source.number);
		case Source::Kind::Controller: return "CC " + std::to_string(_source.number);
		case Source::Kind::None: break;
		}
		return "-";
	}

	std::string describe(const RawMessage& _message)
	{
		const auto type = _message.status & 0xf0;
		const auto channel = " (ch " + std::to_string((_message.status & 0x0f) + 1) + ")";
		const auto d1 = std::to_string(_message.data1);
		const auto d2 = std::to_string(_message.data2);

		if(type == g_statusNoteOn && _message.data2 != 0)
			return "Note On " + d1 + " vel " + d2 + channel;
		if(type == g_statusNoteOn || type == g_statusNoteOff)
			return "Note Off " + d1 + channel;
		if(type == g_statusController)
			return "CC " + d1 + " = " + d2 + channel;
		return "Status " + std::to_string(_message.status) + " " + d1 + " " + d2;
	}

	const char* describe(const EncoderMode _mode)
	{
		switch(_mode)
		{
		case EncoderMode::Absolute: return "Absolute";
		case EncoderMode::RelativeOffset: return "Relative 64";
		case EncoderMode::RelativeTwosComplement: return "Relative 2's";
		}
		return "Absolute";
	}

	bool passesChannel(const Table& _table, const uint8_t _status)
	{
		return _table.channel == 0 || (_status & 0x0f) == _table.channel - 1;
	}

	std::optional<std::string> channelOverlapWarning(const md::MachineModel _model, const uint8_t _panelChannel,
		const uint8_t _baseChannel)
	{
		if(_baseChannel > 15 || _panelChannel > 16)
			return std::nullopt;

		// The machine's channels, counted 1-16 like the panel filter.
		const int first = _baseChannel + 1;
		const int last = std::min(first + machineChannelCount(_model) - 1, 16);

		const bool omni = _panelChannel == 0;
		if(!omni && (_panelChannel < first || _panelChannel > last))
			return std::nullopt;

		const std::string machine = _model == md::MachineModel::Monomachine ? "Monomachine" : "Machinedrum";
		const std::string range = first == last
			? std::to_string(first) : std::to_string(first) + "-" + std::to_string(last);

		// The highest channel outside the block, so the advice is usually "16".
		int suggestion = 0;
		for(int channel = 16; channel >= 1; --channel)
		{
			if(channel < first || channel > last)
			{
				suggestion = channel;
				break;
			}
		}

		std::string text = omni
			? "Warning: Omni includes channels " + range + ", which the " + machine + " itself listens on."
			: "Warning: channel " + std::to_string(_panelChannel) + " is one of the channels the " + machine
				+ " itself listens on (" + range + ").";
		text += " If this controller also reaches the machine's own MIDI input, its messages will change the machine's parameters.";
		if(suggestion != 0)
			text += " Use another channel, for example " + std::to_string(suggestion) + ".";
		return text;
	}

	void Map::setTable(const Table& _table)
	{
		m_table = _table;

		for(auto& t : m_notes)
			t = {};
		for(auto& t : m_controllers)
			t = {};

		const auto bind = [this](const Source& _source, const Target::Kind _kind, const size_t _index)
		{
			if(_source.number > 127)
				return;
			if(_source.kind == Source::Kind::Note)
				m_notes[_source.number] = { _kind, static_cast<uint8_t>(_index) };
			else if(_source.kind == Source::Kind::Controller)
				m_controllers[_source.number] = { _kind, static_cast<uint8_t>(_index) };
		};

		for(size_t i = 0; i < m_table.encoders.size(); ++i)
			if(m_table.encoders[i].source.kind == Source::Kind::Controller)
				bind(m_table.encoders[i].source, Target::Kind::Encoder, i);
		for(size_t i = 0; i < m_table.pushes.size(); ++i)
			bind(m_table.pushes[i], Target::Kind::Push, i);
		for(size_t i = 0; i < m_table.buttons.size(); ++i)
			bind(m_table.buttons[i], Target::Kind::Button, i);

		reset();
	}

	void Map::reset()
	{
		for(auto& last : m_lastAbsolute)
			last = -1;
	}

	std::optional<Action> Map::translate(const RawMessage& _message)
	{
		const auto status = _message.status;
		if(status < 0x80 || status >= 0xf0 || !passesChannel(m_table, status))
			return std::nullopt;

		const auto data1 = static_cast<uint8_t>(_message.data1 & 0x7f);
		const auto data2 = static_cast<uint8_t>(_message.data2 & 0x7f);

		switch(status & 0xf0)
		{
		case g_statusNoteOn:
		case g_statusNoteOff:
			{
				const auto target = m_notes[data1];
				const bool down = (status & 0xf0) == g_statusNoteOn && data2 != 0;
				if(target.kind == Target::Kind::Push)
					return pushAction(target.index, down);
				if(target.kind != Target::Kind::Button)
					return std::nullopt;
				return buttonAction(target.index, down);
			}
		case g_statusController:
			{
				const auto target = m_controllers[data1];
				if(target.kind == Target::Kind::Push)
					return pushAction(target.index, data2 >= 64);
				if(target.kind == Target::Kind::Button)
					return buttonAction(target.index, data2 >= 64);
				if(target.kind != Target::Kind::Encoder)
					return std::nullopt;

				int steps = 0;
				switch(m_table.encoders[target.index].mode)
				{
				case EncoderMode::Absolute:
					{
						const auto last = m_lastAbsolute[target.index];
						m_lastAbsolute[target.index] = data2;
						// The first value only anchors: the controller may sit anywhere.
						if(last >= 0)
							steps = static_cast<int>(data2) - last;
					}
					break;
				case EncoderMode::RelativeOffset:
					steps = static_cast<int>(data2) - 64;
					break;
				case EncoderMode::RelativeTwosComplement:
					if(data2 < 64)
						steps = data2;
					else if(data2 > 64)
						steps = static_cast<int>(data2) - 128;
					break;
				}

				if(steps == 0)
					return std::nullopt;
				return encoderAction(target.index, steps);
			}
		default:
			return std::nullopt;
		}
	}
}
