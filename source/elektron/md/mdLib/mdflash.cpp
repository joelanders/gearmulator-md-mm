#include "mdflash.h"

namespace md
{
	namespace
	{
		constexpr uint32_t g_unlockAddress1 = 0x0000aaaa;
		constexpr uint32_t g_unlockAddress2 = 0x00005554;
		constexpr uint16_t g_unlockValue1 = 0xaaaa;
		constexpr uint16_t g_unlockValue2 = 0x5555;
		constexpr uint16_t g_identifyCommand = 0x9090;
		constexpr uint16_t g_programCommand = 0xa0a0;
		constexpr uint16_t g_eraseCommand = 0x8080;
		constexpr uint16_t g_eraseSectorCommand = 0x3030;
		constexpr uint16_t g_resetCommand = 0xf0f0;
	}

	std::optional<FlashCommandDecoder::Operation> FlashCommandDecoder::write16(
		const uint32_t _offset, const uint16_t _value)
	{
		// Once Program has been armed, every 16-bit value is payload. In
		// particular F0F0 is valid firmware data rather than a reset command.
		if(_value == g_resetCommand && m_state != State::Program)
		{
			m_state = State::ReadArray;
			return std::nullopt;
		}

		switch(m_state)
		{
		case State::ReadArray:
		case State::Identify:
			m_state = _offset == g_unlockAddress1 && _value == g_unlockValue1
				? State::Unlock1 : State::ReadArray;
			break;
		case State::Unlock1:
			m_state = _offset == g_unlockAddress2 && _value == g_unlockValue2
				? State::Unlock2 : State::ReadArray;
			break;
		case State::Unlock2:
			if(_offset != g_unlockAddress1)
				m_state = State::ReadArray;
			else if(_value == g_identifyCommand)
				m_state = State::Identify;
			else if(_value == g_programCommand)
				m_state = State::Program;
			else if(_value == g_eraseCommand)
				m_state = State::EraseUnlock1;
			else
				m_state = State::ReadArray;
			break;
		case State::Program:
			m_state = State::ReadArray;
			return Operation{Operation::Type::ProgramWord, _offset, _value};
		case State::EraseUnlock1:
			m_state = _offset == g_unlockAddress1 && _value == g_unlockValue1
				? State::EraseUnlock2 : State::ReadArray;
			break;
		case State::EraseUnlock2:
			m_state = _offset == g_unlockAddress2 && _value == g_unlockValue2
				? State::EraseCommand : State::ReadArray;
			break;
		case State::EraseCommand:
			m_state = State::ReadArray;
			if(_value == g_eraseSectorCommand)
				return Operation{Operation::Type::EraseSector, _offset, 0xffff};
			break;
		}
		return std::nullopt;
	}

	std::optional<uint16_t> FlashCommandDecoder::read16(const uint32_t _offset) const
	{
		if(m_state != State::Identify)
			return std::nullopt;
		if(_offset == 0)
			return g_manufacturerId;
		if(_offset == 2)
			return g_deviceId;
		return uint16_t{0};
	}

	std::optional<uint8_t> FlashCommandDecoder::read8(const uint32_t _offset) const
	{
		const auto word = read16(_offset & ~uint32_t{1});
		if(!word)
			return std::nullopt;
		return (_offset & 1) ? static_cast<uint8_t>(*word)
			: static_cast<uint8_t>(*word >> 8);
	}
}
