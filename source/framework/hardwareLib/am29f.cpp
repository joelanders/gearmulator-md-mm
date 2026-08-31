#include "am29f.h"

#include <algorithm>
#include <cassert>
#include <limits>

#include "mc68k/logging.h"
#include "mc68k/mc68k.h"

namespace hwLib
{
	Am29f::Am29f(uint8_t* _buffer, const size_t _size, const bool _useWriteEnable,
		const bool _bitreversedCmdAddr)
		: m_buffer(_buffer)
		, m_size(_size)
		, m_useWriteEnable(_useWriteEnable)
		, m_commandAddressAa(_bitreversedCmdAddr
			? static_cast<uint16_t>(bitreverse(0x555) >> 4) : 0x555)
		, m_commandAddress55(_bitreversedCmdAddr
			? static_cast<uint16_t>(bitreverse(0x2aa) >> 4) : 0x2aa)
	{
	}

	bool Am29f::matches(const uint32_t _addr, const uint16_t _data,
		const uint16_t _commandAddress, const uint8_t _commandData) const
	{
		return (_addr & 0xfff) == _commandAddress
			&& static_cast<uint8_t>(_data) == _commandData;
	}

	void Am29f::resetCommand()
	{
		m_state = State::Idle;
	}

	void Am29f::write(const uint32_t _addr, const uint16_t _data)
	{
		if(!writeEnabled())
		{
			resetCommand();
			return;
		}

		switch(m_state)
		{
		case State::Idle:
			if(matches(_addr, _data, m_commandAddressAa, 0xaa))
				m_state = State::Unlock1;
			break;
		case State::Unlock1:
			m_state = matches(_addr, _data, m_commandAddress55, 0x55)
				? State::Command : State::Idle;
			break;
		case State::Command:
			if(matches(_addr, _data, m_commandAddressAa, 0xa0))
				m_state = State::Program;
			else if(matches(_addr, _data, m_commandAddressAa, 0x80))
				m_state = State::EraseUnlock1;
			else
				resetCommand();
			break;
		case State::Program:
			execCommand(CommandType::Program, _addr, _data);
			resetCommand();
			break;
		case State::EraseUnlock1:
			m_state = matches(_addr, _data, m_commandAddressAa, 0xaa)
				? State::EraseUnlock2 : State::Idle;
			break;
		case State::EraseUnlock2:
			m_state = matches(_addr, _data, m_commandAddress55, 0x55)
				? State::EraseCommand : State::Idle;
			break;
		case State::EraseCommand:
			if(matches(_addr, _data, m_commandAddressAa, 0x10))
				execCommand(CommandType::ChipErase, _addr, _data);
			else if(static_cast<uint8_t>(_data) == 0x30)
				execCommand(CommandType::SectorErase, _addr, _data);
			resetCommand();
			break;
		}
	}

	bool Am29f::eraseSector(const uint32_t _addr, const size_t _sizeInKb) const
	{
		if(!_sizeInKb || _sizeInKb > std::numeric_limits<size_t>::max() / 1024)
			return false;
		const auto size = _sizeInKb * 1024;
		if(_addr > m_size || size > m_size - _addr)
			return false;

		MCLOG("Erasing Sector at " << MCHEX(_addr) << ", size " << MCHEX(size));
		std::fill_n(m_buffer + _addr, size, uint8_t{0xff});

		return true;
	}

	bool Am29f::eraseSector1Mbit(const uint32_t _addr) const
	{
		switch (_addr)
		{
			case 0x00000:
			case 0x04000:
			case 0x08000:
			case 0x0C000:
			case 0x10000:
			case 0x14000:
			case 0x18000:
			case 0x1C000:	return eraseSector(_addr, 16);
			default:		return false;
		}
	}

	bool Am29f::eraseSector2MbitTopBoot(const uint32_t _addr) const
	{
		switch (_addr)
		{
			case 0x00000:
			case 0x10000:
			case 0x20000:	return eraseSector(_addr, 64);
			case 0x30000:	return eraseSector(_addr, 32);
			case 0x38000:
			case 0x3a000:	return eraseSector(_addr, 8);
			case 0x3c000:	return eraseSector(_addr, 16);
			default:		return false;
		}
	}

	bool Am29f::eraseSector4MbitTopBoot(const uint32_t _addr) const
	{
		switch (_addr)
		{
			case 0x00000:
			case 0x10000:
			case 0x20000:
			case 0x30000:
			case 0x40000:
			case 0x50000:
			case 0x60000:
			case 0x70000:	return eraseSector(_addr, 64);
			case 0x78000:
			case 0x7a000:	return eraseSector(_addr, 8);
			case 0x7c000:	return eraseSector(_addr, 16);
			default:		return false;
		}
	}

	void Am29f::execCommand(const CommandType _command, uint32_t _addr, const uint16_t _data) const
	{
		switch (_command)
		{
		case CommandType::ChipErase:
			std::fill_n(m_buffer, m_size, uint8_t{0xff});
			break;
		case CommandType::SectorErase:
			{
				if (!eraseSector(_addr))
				{
					assert(false);
					MCLOG("Unable to erase sector at " << MCHEX(_addr) << ", unable to determine sector size!");
				}
			}
			break;
		case CommandType::Program:
			{
				if(_addr >= m_size || m_size - _addr < sizeof(uint16_t))
					return;
#if defined(_DEBUG) && defined(_WIN32)
				MCLOG("Programming word at " << MCHEX(_addr) << ", value " << MCHEXN(_data, 4));
#endif
				const auto old = mc68k::memoryOps::readU16(m_buffer, _addr);
				// "A bit cannot be programmed from a 0 back to a 1"
				const auto v = _data & old;
				mc68k::memoryOps::writeU16(m_buffer, _addr, v);
	//			assert(v == _data);
				break;
			}
		case CommandType::Invalid: 
		default: 
			assert(false);
			break;
		}
	}
}
