#include "mdPanelMidiController.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "baseLib/filesystem.h"

namespace mdJucePlugin::panelMidi
{
	Controller::Controller(const md::MachineModel _model, std::string _filePath, ActionHandler _handler)
		: m_model(_model)
		, m_filePath(std::move(_filePath))
		, m_handler(std::move(_handler))
	{
		m_map.setTable(makeDefaultTable(m_model));
		load();
	}

	void Controller::load()
	{
		if(m_filePath.empty())
			return;

		std::ifstream file(m_filePath, std::ios::binary);
		if(!file)
			return;

		std::stringstream text;
		text << file.rdbuf();

		Table table;
		if(fromText(text.str(), m_model, table))
			m_map.setTable(table);
	}

	bool Controller::save() const
	{
		if(m_filePath.empty())
			return false;

		// getPath returns the whole name when there is no folder part.
		if(m_filePath.find_last_of("/\\") != std::string::npos)
			baseLib::filesystem::createDirectory(baseLib::filesystem::getPath(m_filePath));

		std::ofstream file(m_filePath, std::ios::binary | std::ios::trunc);
		if(!file)
			return false;
		file << toText(m_map.getTable());
		return static_cast<bool>(file);
	}

	void Controller::replaceTable(const Table& _table)
	{
		m_map.setTable(_table);
		++m_revision;
		save();
	}

	void Controller::handleMessage(const RawMessage& _message)
	{
		// Same rule as the input: what cannot be bound is not shown or learned.
		if(!isBindableMessage(_message.status))
			return;

		m_lastMessage = _message;
		++m_messageSerial;
		m_lastMessageFiltered = _message.status >= 0x80 && _message.status < 0xf0
			&& !passesChannel(getTable(), _message.status);

		// While learning, the controller is being pointed at a control, not played.
		if(m_learn.active())
		{
			(void)tryLearn(_message);
			return;
		}

		if(const auto action = m_map.translate(_message))
			if(m_handler)
				m_handler(*action);
	}

	bool Controller::tryLearn(const RawMessage& _message)
	{
		const auto status = _message.status;
		if(status < 0x80 || status >= 0xf0 || !passesChannel(getTable(), status))
			return false;

		const auto type = status & 0xf0;
		const auto number = static_cast<uint8_t>(_message.data1 & 0x7f);

		Source source;
		if(type == 0x90 && _message.data2 != 0)
			source = { Source::Kind::Note, number };
		else if(type == 0xb0)
			source = { Source::Kind::Controller, number };
		else
			return false;

		// Encoders turn from controllers only; a note cannot drive one.
		if(m_learn.kind == LearnTarget::Kind::Encoder && source.kind != Source::Kind::Controller)
			return false;

		// Within a run, what was already learned must not be learned again.
		if(!m_sequence.empty())
			for(const auto& learned : m_sequenceLearned)
				if(learned == source)
					return false;

		auto table = getTable();

		for(auto& encoder : table.encoders)
			if(encoder.source == source)
				encoder.source = {};
		for(auto& push : table.pushes)
			if(push == source)
				push = {};
		for(auto& button : table.buttons)
			if(button == source)
				button = {};

		if(m_learn.kind == LearnTarget::Kind::Encoder)
			table.encoders[static_cast<size_t>(m_learn.encoder)].source = source;
		else if(m_learn.kind == LearnTarget::Kind::EncoderPush)
			table.pushes[static_cast<size_t>(m_learn.encoder)] = source;
		else
			table.buttons[static_cast<size_t>(m_learn.control)] = source;

		if(m_sequence.empty())
		{
			m_learn = {};
		}
		else
		{
			m_sequenceLearned.push_back(source);
			if(++m_sequenceIndex < m_sequence.size())
				m_learn = m_sequence[m_sequenceIndex];
			else
				endSequence();
		}

		replaceTable(table);
		return true;
	}

	void Controller::endSequence()
	{
		m_sequence.clear();
		m_sequenceLearned.clear();
		m_sequenceIndex = 0;
		m_learn = {};
	}

	void Controller::beginLearnSequence(const std::vector<LearnTarget>& _targets)
	{
		std::vector<LearnTarget> available;
		for(const auto& target : _targets)
		{
			switch(target.kind)
			{
			case LearnTarget::Kind::Encoder:
				if(isAvailable(m_model, target.encoder))
					available.push_back(target);
				break;
			case LearnTarget::Kind::EncoderPush:
				if(isPushAvailable(m_model, target.encoder))
					available.push_back(target);
				break;
			case LearnTarget::Kind::Button:
				if(isAvailable(m_model, target.control))
					available.push_back(target);
				break;
			case LearnTarget::Kind::None:
				break;
			}
		}
		if(available.empty())
			return;

		endSequence();
		m_sequence = std::move(available);
		m_learn = m_sequence.front();
		++m_revision;
	}

	void Controller::setChannel(const uint8_t _channel)
	{
		auto table = getTable();
		table.channel = std::min<uint8_t>(_channel, 16);
		if(table != getTable())
			replaceTable(table);
	}

	void Controller::setEncoderMode(const md::PanelEncoder _encoder, const EncoderMode _mode)
	{
		if(!isAvailable(m_model, _encoder))
			return;
		auto table = getTable();
		table.encoders[static_cast<size_t>(_encoder)].mode = _mode;
		if(table != getTable())
			replaceTable(table);
	}

	void Controller::clear(const md::PanelEncoder _encoder)
	{
		auto table = getTable();
		table.encoders[static_cast<size_t>(_encoder)].source = {};
		if(table != getTable())
			replaceTable(table);
	}

	void Controller::clear(const md::PanelControl _control)
	{
		auto table = getTable();
		table.buttons[static_cast<size_t>(_control)] = {};
		if(table != getTable())
			replaceTable(table);
	}

	void Controller::clearPush(const md::PanelEncoder _encoder)
	{
		if(static_cast<size_t>(_encoder) >= g_pushCount)
			return;
		auto table = getTable();
		table.pushes[static_cast<size_t>(_encoder)] = {};
		if(table != getTable())
			replaceTable(table);
	}

	void Controller::resetToDefault()
	{
		endSequence();
		replaceTable(makeDefaultTable(m_model));
	}

	void Controller::beginLearn(const md::PanelEncoder _encoder)
	{
		if(!isAvailable(m_model, _encoder))
			return;
		endSequence();
		m_learn.kind = LearnTarget::Kind::Encoder;
		m_learn.encoder = _encoder;
		++m_revision;
	}

	void Controller::beginLearnPush(const md::PanelEncoder _encoder)
	{
		if(static_cast<size_t>(_encoder) >= g_pushCount || !isPushAvailable(m_model, _encoder))
			return;
		endSequence();
		m_learn.kind = LearnTarget::Kind::EncoderPush;
		m_learn.encoder = _encoder;
		++m_revision;
	}

	void Controller::beginLearn(const md::PanelControl _control)
	{
		if(!isAvailable(m_model, _control))
			return;
		endSequence();
		m_learn.kind = LearnTarget::Kind::Button;
		m_learn.control = _control;
		++m_revision;
	}

	void Controller::cancelLearn()
	{
		if(!m_learn.active())
			return;
		endSequence();
		++m_revision;
	}
}
