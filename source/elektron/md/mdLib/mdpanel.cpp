#include "mdpanel.h"

#include <algorithm>

namespace md
{
	namespace
	{
		constexpr bool isTrigger(const PanelControl _control)
		{
			return _control >= PanelControl::Trigger1
				&& _control <= PanelControl::Trigger16;
		}

		constexpr bool isTrack(const PanelControl _control)
		{
			return _control >= PanelControl::Track1
				&& _control <= PanelControl::Track6;
		}

		constexpr uint8_t controlIndex(const PanelControl _control,
			const PanelControl _first)
		{
			return static_cast<uint8_t>(static_cast<uint8_t>(_control)
				- static_cast<uint8_t>(_first));
		}

		constexpr PanelPacket packet(const uint8_t _row, const uint8_t _mask)
		{
			return {_row, _mask};
		}
	}

	std::optional<PanelPacket> panelPacket(const MachineModel _model,
		const PanelControl _control)
	{
		if(isTrigger(_control))
		{
			const uint8_t index = controlIndex(_control, PanelControl::Trigger1);
			return packet(static_cast<uint8_t>(0x20 + (index >> 3)),
				static_cast<uint8_t>(1u << (index & 7)));
		}

		if(_model == MachineModel::Monomachine && isTrack(_control))
		{
			const uint8_t index = controlIndex(_control, PanelControl::Track1);
			return packet(0x22, static_cast<uint8_t>(0x80u >> index));
		}

		if(_model == MachineModel::Machinedrum)
		{
			switch(_control)
			{
			case PanelControl::Tempo: return packet(0x22, 0x01);
			case PanelControl::SynthesisEffectsRouting: return packet(0x22, 0x10);
			case PanelControl::Record: return packet(0x22, 0x02);
			case PanelControl::Play: return packet(0x22, 0x04);
			case PanelControl::Stop: return packet(0x22, 0x08);
			case PanelControl::PatternSong: return packet(0x22, 0x20);
			case PanelControl::Kit: return packet(0x22, 0x40);
			case PanelControl::Scale: return packet(0x22, 0x80);
			case PanelControl::BankA: return packet(0x23, 0x01);
			case PanelControl::BankB: return packet(0x23, 0x02);
			case PanelControl::BankC: return packet(0x23, 0x04);
			case PanelControl::BankD: return packet(0x23, 0x08);
			case PanelControl::Exit: return packet(0x23, 0x10);
			case PanelControl::Left: return packet(0x23, 0x20);
			case PanelControl::Down: return packet(0x23, 0x40);
			case PanelControl::Right: return packet(0x23, 0x80);
			case PanelControl::ClassicExtended: return packet(0x24, 0x01);
			case PanelControl::Function: return packet(0x24, 0x02);
			case PanelControl::BankGroup: return packet(0x24, 0x04);
			case PanelControl::Enter: return packet(0x24, 0x08);
			case PanelControl::Up: return packet(0x24, 0x10);
			default: return std::nullopt;
			}
		}

		switch(_control)
		{
		case PanelControl::Tempo: return packet(0x23, 0x01);
		case PanelControl::Function: return packet(0x23, 0x02);
		case PanelControl::Kit: return packet(0x23, 0x04); // MM: KIT/SONG SETUP.
		case PanelControl::Enter: return packet(0x23, 0x10);
		case PanelControl::Exit: return packet(0x23, 0x20);
		case PanelControl::Up: return packet(0x23, 0x40);
		case PanelControl::Left: return packet(0x23, 0x80);
		case PanelControl::Down: return packet(0x24, 0x01);
		case PanelControl::Right: return packet(0x24, 0x02);
		case PanelControl::BankGroup: return packet(0x24, 0x04);
		case PanelControl::BankA: return packet(0x24, 0x08);
		case PanelControl::BankB: return packet(0x24, 0x10);
		case PanelControl::BankC: return packet(0x24, 0x20);
		case PanelControl::BankD: return packet(0x24, 0x40);
		case PanelControl::Record: return packet(0x24, 0x80);
		case PanelControl::Play: return packet(0x25, 0x01);
		case PanelControl::Stop: return packet(0x25, 0x02);
		case PanelControl::DataPageForward: return packet(0x25, 0x04);
		case PanelControl::DataPageBackward: return packet(0x25, 0x08);
		case PanelControl::Scale: return packet(0x22, 0x01);
		case PanelControl::TrigSelect: return packet(0x22, 0x02);
		case PanelControl::SongEnable: return packet(0x23, 0x08);
		default: return std::nullopt;
		}
	}

	std::optional<uint8_t> panelEncoderCommand(const MachineModel _model,
		const PanelEncoder _encoder)
	{
		const auto index = static_cast<uint8_t>(_encoder);
		if(index <= static_cast<uint8_t>(PanelEncoder::DataEntryH))
			return static_cast<uint8_t>(0x30 + index);
		if(_encoder == PanelEncoder::Level)
			return 0x38;
		if(_encoder == PanelEncoder::SoundSelection &&
			_model == MachineModel::Machinedrum)
			return 0x39;
		return std::nullopt;
	}

	PanelPacket PanelRowState::press(const PanelPacket& _packet)
	{
		if(_packet.row < g_firstRow || _packet.row > g_lastRow)
			return _packet;
		auto& rowMask = m_masks[_packet.row - g_firstRow];
		rowMask = static_cast<uint8_t>(rowMask | _packet.mask);
		return {_packet.row, rowMask};
	}

	PanelPacket PanelRowState::release(const PanelPacket& _packet)
	{
		if(_packet.row < g_firstRow || _packet.row > g_lastRow)
			return _packet;
		auto& rowMask = m_masks[_packet.row - g_firstRow];
		rowMask = static_cast<uint8_t>(rowMask & ~_packet.mask);
		return {_packet.row, rowMask};
	}

	uint8_t PanelRowState::mask(const uint8_t _row) const
	{
		return _row < g_firstRow || _row > g_lastRow
			? 0 : m_masks[_row - g_firstRow];
	}

	void PanelRowState::reset()
	{
		m_masks.fill(0);
	}

	PanelInputQueue::PanelInputQueue()
	{
		for(size_t i = 0; i < m_slots.size(); ++i)
			m_slots[i].sequence.store(i, std::memory_order_relaxed);
	}

	bool PanelInputQueue::tryPush(const uint8_t _command, const uint8_t _argument)
	{
		auto position = m_enqueuePosition.load(std::memory_order_relaxed);
		for(;;)
		{
			auto& slot = m_slots[position % m_slots.size()];
			const auto sequence = slot.sequence.load(std::memory_order_acquire);
			const auto difference = static_cast<std::ptrdiff_t>(sequence)
				- static_cast<std::ptrdiff_t>(position);
			if(difference == 0)
			{
				if(!m_enqueuePosition.compare_exchange_weak(position, position + 1,
					std::memory_order_relaxed))
					continue;

				// Count the claimed slot before publishing it. The consumer cannot see
				// sequence == position + 1 until this increment and packet write finish.
				// Release the counted-work wake before publishing the slot. Hardware's
				// acquire precheck can therefore skip the empty queue without a second
				// dirty flag; the slot sequence release/acquire below still protects the
				// packet itself. This mirrors Gearmulator's counted semaphore pattern.
				m_pendingPackets.fetch_add(1, std::memory_order_release);
				slot.packet = {_command, _argument};
				slot.sequence.store(position + 1, std::memory_order_release);
				return true;
			}
			if(difference < 0)
			{
				m_overflowCount.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			position = m_enqueuePosition.load(std::memory_order_relaxed);
		}
	}

	size_t PanelInputQueue::size() const
	{
		return pendingPackets() * 2;
	}

	size_t PanelInputQueue::pendingPackets() const
	{
		return m_pendingPackets.load(std::memory_order_acquire);
	}

	size_t PanelInputQueue::overflowCount() const
	{
		return m_overflowCount.load(std::memory_order_relaxed);
	}

	size_t PanelInputQueue::drain(DrainBuffer& _destination,
		const size_t _maximumPackets)
	{
		const auto limit = std::min(_maximumPackets, _destination.size());
		size_t count = 0;
		while(count < limit)
		{
			auto& slot = m_slots[m_dequeuePosition % m_slots.size()];
			const auto sequence = slot.sequence.load(std::memory_order_acquire);
			if(sequence != m_dequeuePosition + 1)
				break;

			_destination[count++] = slot.packet;
			slot.sequence.store(m_dequeuePosition + m_slots.size(),
				std::memory_order_release);
			++m_dequeuePosition;
			m_pendingPackets.fetch_sub(1, std::memory_order_relaxed);
		}
		return count;
	}
}
