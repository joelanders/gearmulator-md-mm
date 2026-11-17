#include "mdPanelMidiDriver.h"

#include <algorithm>
#include <cstdlib>

namespace mdJucePlugin::panelMidi
{
	Driver::Driver(const md::MachineModel _model, md::PanelRowMixer& _rows, SendFunc _send)
		: m_model(_model)
		, m_rows(_rows.port(md::PanelInputSource::Midi))
		, m_send(std::move(_send))
	{
		for(size_t i = 0; i < g_controlCount; ++i)
			m_keys[i].packet = md::panelPacket(_model, static_cast<md::PanelControl>(i));
		for(size_t i = 0; i < g_pushCount; ++i)
			m_keys[g_controlCount + i].packet = md::panelEncoderPressPacket(_model, static_cast<md::PanelEncoder>(i));
	}

	void Driver::apply(const Action& _action, const double _nowMs)
	{
		switch(_action.kind)
		{
		case Action::Kind::ButtonDown:
		case Action::Kind::ButtonUp:
			queueEdge(static_cast<size_t>(_action.control), _action.kind == Action::Kind::ButtonDown, _nowMs);
			break;
		case Action::Kind::EncoderPushDown:
		case Action::Kind::EncoderPushUp:
			if(static_cast<size_t>(_action.encoder) < g_pushCount)
				queueEdge(g_controlCount + static_cast<size_t>(_action.encoder),
					_action.kind == Action::Kind::EncoderPushDown, _nowMs);
			break;
		case Action::Kind::EncoderSteps:
			{
				const auto command = md::panelEncoderCommand(m_model, _action.encoder);
				if(!command || _action.steps == 0)
					return;
				const auto steps = std::clamp(_action.steps, -g_encoderBurstCap, g_encoderBurstCap);
				const auto argument = static_cast<uint8_t>(steps > 0 ? 0x01 : 0xff);
				for(int i = 0; i < std::abs(steps); ++i)
					(void)m_send(*command, argument);
				if(m_encoderTurned)
					m_encoderTurned(_action.encoder, steps);
			}
			break;
		}
	}

	void Driver::queueEdge(const size_t _key, const bool _down, const double _nowMs)
	{
		if(_key >= m_keys.size())
			return;
		auto& key = m_keys[_key];
		if(!key.packet)
			return;

		// Repeats of the state the key is already headed for change nothing.
		const auto last = key.pending.empty() ? key.down : key.pending.back();
		if(last == _down || key.pending.size() >= g_maxPendingEdges)
			return;

		key.pending.push_back(_down);
		service(_nowMs);
	}

	void Driver::service(const double _nowMs)
	{
		for(auto& key : m_keys)
		{
			if(key.pending.empty())
				continue;
			if(key.everSent && _nowMs - key.lastEdgeMs < g_minEdgeIntervalMs)
				continue;
			const auto down = key.pending.front();
			key.pending.pop_front();
			sendEdge(key, down, _nowMs);
		}
	}

	void Driver::sendEdge(Key& _key, const bool _down, const double _nowMs)
	{
		const auto combined = _down ? m_rows.press(*_key.packet) : m_rows.release(*_key.packet);
		(void)m_send(combined.row, combined.mask);
		_key.down = _down;
		_key.everSent = true;
		_key.lastEdgeMs = _nowMs;
	}

	void Driver::releaseAll(const double _nowMs)
	{
		for(auto& key : m_keys)
		{
			key.pending.clear();
			if(key.down)
				sendEdge(key, false, _nowMs);
		}
	}

	bool Driver::isHeld(const md::PanelControl _control) const
	{
		const auto index = static_cast<size_t>(_control);
		return index < g_controlCount && m_keys[index].down;
	}

	bool Driver::isPushHeld(const md::PanelEncoder _encoder) const
	{
		const auto index = static_cast<size_t>(_encoder);
		return index < g_pushCount && m_keys[g_controlCount + index].down;
	}

	bool Driver::hasPendingEdges() const
	{
		return std::any_of(m_keys.begin(), m_keys.end(), [](const Key& _key) { return !_key.pending.empty(); });
	}
}
