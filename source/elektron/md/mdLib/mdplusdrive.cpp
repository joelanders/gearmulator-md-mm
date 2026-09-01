#include "mdplusdrive.h"

#include <algorithm>

namespace md
{
	namespace
	{
		constexpr uint8_t g_clock = 0x04;
		constexpr uint8_t g_command = 0x08;
		constexpr std::array<uint8_t, 4> g_storageMagic{{'M', 'D', 'P', 'D'}};
		constexpr uint32_t g_storageVersion = 1;
		constexpr size_t g_storageHeaderSize = 16;
		constexpr size_t g_storageRecordSize = 4 + 512;
		constexpr uint32_t g_sectorCount = 2u * 1024u * 1024u * 1024u / 512u;

		uint32_t readBe32(const uint8_t* const _data)
		{
			return (static_cast<uint32_t>(_data[0]) << 24)
				| (static_cast<uint32_t>(_data[1]) << 16)
				| (static_cast<uint32_t>(_data[2]) << 8) | _data[3];
		}

		void appendBe32(std::vector<uint8_t>& _data, const uint32_t _value)
		{
			_data.push_back(static_cast<uint8_t>(_value >> 24));
			_data.push_back(static_cast<uint8_t>(_value >> 16));
			_data.push_back(static_cast<uint8_t>(_value >> 8));
			_data.push_back(static_cast<uint8_t>(_value));
		}

		// Stable, standards-shaped identity values. The OS uses the CSD geometry and
		// OCR capacity flags; product text and serial fields are informational.
		constexpr std::array<uint8_t, 16> g_cid{{
			0x15, 0x01, 0x00, 'G', 'M', 'D', '+', 'D', 'R', 0x01,
			0x12, 0x34, 0x56, 0x78, 0x01, 0xff}};
		constexpr std::array<uint8_t, 16> g_csd{{
			0xd0, 0x0f, 0x00, 0x32, 0x0f, 0x59, 0x00, 0x00,
			0xed, 0xc8, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0xff}};
	}

	void PlusDrive::reset()
	{
		m_clock = false;
		m_commandOutput = true;
		m_command.fill(0);
		m_commandBits = 0;
		m_response.fill(0xff);
		m_responseBits = 0;
		m_responseCursor = 0;
		m_responseDelay = 0;
		m_initialized = false;
		m_reading = false;
		m_singleBlock = false;
		m_dataDelay = 0;
		m_dataCursor = 0;
		m_dataOutput = 0xf0;
		m_readSector = 0;
		m_writeState = WriteState::Idle;
		m_singleWrite = false;
		m_writeSector = 0;
		m_writeCursor = 0;
		m_writeResponseCursor = 0;
		m_writeBlock.fill(0xff);
		m_writeSawIdleData = false;
		m_commandCount = 0;
		m_lastCommand = 0xff;
	}

	void PlusDrive::setEnabled(const bool _enabled)
	{
		if(m_enabled == _enabled)
			return;
		m_enabled = _enabled;
		reset();
	}

	std::vector<uint8_t> PlusDrive::copyStorage() const
	{
		if(!m_serializedStorage.empty())
			return m_serializedStorage;
		std::vector<uint32_t> sectors;
		sectors.reserve(m_sectors.size());
		for(const auto& entry : m_sectors)
			sectors.push_back(entry.first);
		std::sort(sectors.begin(), sectors.end());

		std::vector<uint8_t> result;
		result.reserve(g_storageHeaderSize + sectors.size() * g_storageRecordSize);
		result.insert(result.end(), g_storageMagic.begin(), g_storageMagic.end());
		appendBe32(result, g_storageVersion);
		appendBe32(result, 512);
		appendBe32(result, static_cast<uint32_t>(sectors.size()));
		for(const auto sector : sectors)
		{
			appendBe32(result, sector);
			const auto& data = m_sectors.at(sector);
			result.insert(result.end(), data.begin(), data.end());
		}
		m_serializedStorage = result;
		return result;
	}

	bool PlusDrive::validateStorage(const uint8_t* const _data, const size_t _size)
	{
		if(_size == 0)
			return true;
		if(_size < g_storageHeaderSize || !_data
			|| !std::equal(g_storageMagic.begin(), g_storageMagic.end(), _data)
			|| readBe32(_data + 4) != g_storageVersion
			|| readBe32(_data + 8) != 512)
			return false;
		const uint32_t count = readBe32(_data + 12);
		if(count > g_sectorCount
			|| _size != g_storageHeaderSize
				+ static_cast<size_t>(count) * g_storageRecordSize)
			return false;
		if(count == 0)
			return true;

		std::vector<uint8_t> seen((g_sectorCount + 7u) / 8u);
		size_t offset = g_storageHeaderSize;
		for(uint32_t i = 0; i < count; ++i, offset += g_storageRecordSize)
		{
			const uint32_t sector = readBe32(_data + offset);
			if(sector >= g_sectorCount)
				return false;
			const auto mask = static_cast<uint8_t>(1u << (sector & 7u));
			auto& byte = seen[sector >> 3u];
			if(byte & mask)
				return false;
			byte |= mask;
		}
		return true;
	}

	bool PlusDrive::isBlankStorage(const std::vector<uint8_t>& _data)
	{
		return _data.empty() || (validateStorage(_data)
			&& _data.size() == g_storageHeaderSize
			&& readBe32(_data.data() + 12) == 0);
	}

	bool PlusDrive::replaceStorage(const std::vector<uint8_t>& _data, const bool _dirty)
	{
		if(_data.empty())
		{
			if(!m_sectors.empty())
			{
				m_sectors.clear();
				storageChanged(_dirty);
			}
			else if(_dirty && !storageDirty())
				storageChanged(true);
			else if(!_dirty)
				m_persistedGeneration = m_storageGeneration;
			return true;
		}
		if(!validateStorage(_data))
			return false;
		const uint32_t count = readBe32(_data.data() + 12);

		std::unordered_map<uint32_t, std::array<uint8_t, 512>> sectors;
		sectors.reserve(count);
		size_t offset = g_storageHeaderSize;
		for(uint32_t i = 0; i < count; ++i, offset += g_storageRecordSize)
		{
			const uint32_t sector = readBe32(_data.data() + offset);
			std::array<uint8_t, 512> block{};
			std::copy_n(_data.begin() + static_cast<std::ptrdiff_t>(offset + 4),
				block.size(), block.begin());
			sectors.emplace(sector, block);
		}
		if(m_sectors != sectors)
		{
			m_sectors = std::move(sectors);
			storageChanged(_dirty);
		}
		else if(_dirty && !storageDirty())
			storageChanged(true);
		else if(!_dirty)
			m_persistedGeneration = m_storageGeneration;
		return true;
	}

	void PlusDrive::storageChanged(const bool _dirty)
	{
		m_serializedStorage.clear();
		if(++m_storageGeneration == 0)
		{
			m_storageGeneration = 1;
			m_persistedGeneration = 0;
		}
		if(!_dirty)
			m_persistedGeneration = m_storageGeneration;
	}

	void PlusDrive::markStoragePersisted(const uint64_t _generation)
	{
		if(_generation > m_persistedGeneration && _generation <= m_storageGeneration)
			m_persistedGeneration = _generation;
	}

	void PlusDrive::portChanged(const uint8_t _direction, const uint8_t _output)
	{
		if(!m_enabled || !(_direction & g_clock))
		{
			m_clock = false;
			return;
		}
		if(m_writeState == WriteState::AwaitStart && (_direction & 0xf0) == 0xf0)
		{
			if((_output & 0xf0) == 0xf0)
				m_writeSawIdleData = true;
			else if((_output & 0xf0) == 0 && m_writeSawIdleData)
			{
				m_writeState = WriteState::Data;
				m_writeCursor = 0;
				m_writeBlock.fill(0xff);
				// This transition carries the four-bit start token. Data begins on
				// the following edge, so consume the clock phase without sampling it.
				m_clock = (_output & g_clock) != 0;
				return;
			}
		}

		const bool clock = (_output & g_clock) != 0;
		if(clock != m_clock)
			clockEdge(_direction, _output);
		m_clock = clock;
	}

	uint8_t PlusDrive::inputLevel() const
	{
		if(!m_enabled)
			return 0xff;
		// Data lines are pulled high while the block-transfer engine is idle.
		return static_cast<uint8_t>(0x07 | m_dataOutput
			| (m_commandOutput ? g_command : 0));
	}

	void PlusDrive::clockEdge(const uint8_t _direction, const uint8_t _output)
	{
		if(_direction & g_command)
		{
			m_commandOutput = true;
			receiveCommandBit((_output & g_command) != 0);
			return;
		}

		if(m_responseDelay)
		{
			--m_responseDelay;
			m_commandOutput = true;
		}
		else if(m_responseCursor < m_responseBits)
		{
			const size_t byte = m_responseCursor / 8;
			const size_t bit = 7 - (m_responseCursor % 8);
			m_commandOutput = ((m_response[byte] >> bit) & 1) != 0;
			++m_responseCursor;
		}
		else
			m_commandOutput = true;
		advanceReadData(_direction);
		advanceWriteData(_direction, _output);
	}

	void PlusDrive::receiveCommandBit(const bool _bit)
	{
		if(!m_commandBits)
		{
			// MMC commands begin with a zero start bit. Ignore idle clocks.
			if(_bit)
				return;
			m_command.fill(0);
		}

		const size_t byte = m_commandBits / 8;
		m_command[byte] = static_cast<uint8_t>((m_command[byte] << 1) | (_bit ? 1 : 0));
		if(++m_commandBits == 48)
		{
			m_commandBits = 0;
			executeCommand();
		}
	}

	void PlusDrive::executeCommand()
	{
		// The top two bits are the MMC start/transmission framing; the command index
		// occupies the lower six bits.
		m_lastCommand = static_cast<uint8_t>(m_command[0] & 0x3f);
		const uint32_t argument = (static_cast<uint32_t>(m_command[1]) << 24)
			| (static_cast<uint32_t>(m_command[2]) << 16)
			| (static_cast<uint32_t>(m_command[3]) << 8) | m_command[4];
		++m_commandCount;
		switch(m_lastCommand)
		{
		case 0: // GO_IDLE_STATE: no response
			m_initialized = false;
			m_responseBits = 0;
			m_responseCursor = 0;
			m_responseDelay = 0;
			break;
		case 1: // SEND_OP_COND: ready, sector-addressed, supported voltage window
			queueShortResponse(0xc0ff8080u);
			break;
		case 2: // ALL_SEND_CID
			queueLongResponse(g_cid);
			break;
		case 3: // SET_RELATIVE_ADDR
			queueShortResponse(0x00000700u); // ready, STANDBY state
			break;
		case 6: // SWITCH
		case 7: // SELECT_CARD
		case 16: // SET_BLOCKLEN
			queueShortResponse(0x00000900u); // ready, TRANSFER state
			if(m_lastCommand == 16)
				m_initialized = true;
			break;
		case 9: // SEND_CSD
			queueLongResponse(g_csd);
			break;
		case 12: // STOP_TRANSMISSION
			m_reading = false;
			m_writeState = WriteState::Idle;
			m_dataOutput = 0xf0;
			queueShortResponse(0x00000900u);
			break;
		case 17: // READ_SINGLE_BLOCK
		case 18: // READ_MULTIPLE_BLOCK
			if(argument >= g_sectorCount)
			{
				queueShortResponse(0x80000900u); // OUT_OF_RANGE, TRANSFER state
				m_reading = false;
				m_writeState = WriteState::Idle;
				break;
			}
			queueShortResponse(0x00000900u);
			m_writeState = WriteState::Idle;
			m_reading = true;
			m_singleBlock = m_lastCommand == 17;
			m_readSector = argument;
			// R1 turnaround + 40 response bits + the host's trailing clocks.
			m_dataDelay = 52;
			m_dataCursor = 0;
			m_dataOutput = 0xf0;
			break;
		case 24: // WRITE_BLOCK
		case 25: // WRITE_MULTIPLE_BLOCK
			if(argument >= g_sectorCount)
			{
				queueShortResponse(0x80000900u); // OUT_OF_RANGE, TRANSFER state
				m_reading = false;
				m_writeState = WriteState::Idle;
				break;
			}
			queueShortResponse(0x00000900u);
			m_reading = false;
			m_writeState = WriteState::AwaitStart;
			m_singleWrite = m_lastCommand == 24;
			m_writeSector = argument;
			m_writeCursor = 0;
			m_writeResponseCursor = 0;
			m_writeSawIdleData = false;
			m_writeBlock.fill(0xff);
			break;
		default:
			// R1 with ILLEGAL_COMMAND. This keeps unsupported operations bounded and
			// visible to the guest instead of leaving the command line floating.
			queueShortResponse(1u << 22);
			break;
		}
	}

	void PlusDrive::advanceReadData(const uint8_t _direction)
	{
		if(!m_reading || (_direction & 0xf0) != 0)
			return;
		if(m_dataDelay)
		{
			--m_dataDelay;
			m_dataOutput = 0xf0;
			return;
		}

		constexpr size_t dataNibbles = 512 * 2;
		constexpr size_t crcNibbles = 16;
		if(m_dataCursor == 0)
			m_dataOutput = 0x00; // four-bit start token
		else if(m_dataCursor <= dataNibbles)
		{
			const size_t nibble = m_dataCursor - 1;
			const auto sector = m_sectors.find(m_readSector);
			const uint8_t byte = sector == m_sectors.end()
				? 0xff : sector->second[nibble / 2];
			m_dataOutput = static_cast<uint8_t>((nibble & 1)
				? (byte << 4) : (byte & 0xf0));
		}
		else if(m_dataCursor <= dataNibbles + crcNibbles)
			m_dataOutput = 0x00; // one CRC16 per data line
		else
		{
			m_dataOutput = 0xf0;
			if(m_singleBlock)
				m_reading = false;
			else
			{
				if(m_readSector + 1 >= g_sectorCount)
				{
					m_reading = false;
					return;
				}
				++m_readSector;
				m_dataDelay = 2;
				m_dataCursor = 0;
				return;
			}
		}
		++m_dataCursor;
	}

	void PlusDrive::advanceWriteData(const uint8_t _direction, const uint8_t _output)
	{
		if(m_writeState == WriteState::Idle)
			return;

		const bool hostDrivesData = (_direction & 0xf0) == 0xf0;
		if(hostDrivesData)
		{
			const uint8_t nibble = static_cast<uint8_t>(_output >> 4);
			switch(m_writeState)
			{
			case WriteState::AwaitStart:
				break;
			case WriteState::Data:
			{
				const size_t byte = m_writeCursor / 2;
				if((m_writeCursor & 1) == 0)
					m_writeBlock[byte] = static_cast<uint8_t>(nibble << 4);
				else
					m_writeBlock[byte] |= nibble;
				if(++m_writeCursor == 512 * 2)
				{
					m_writeState = WriteState::Crc;
					m_writeCursor = 0;
				}
				break;
			}
			case WriteState::Crc:
				if(++m_writeCursor == 16)
				{
					const auto existing = m_sectors.find(m_writeSector);
					const bool erased = std::all_of(m_writeBlock.begin(), m_writeBlock.end(),
						[](const uint8_t _byte) { return _byte == 0xff; });
					if(erased)
					{
						if(existing != m_sectors.end())
						{
							m_sectors.erase(existing);
							storageChanged(true);
						}
					}
					else if(existing == m_sectors.end() || existing->second != m_writeBlock)
					{
						m_sectors[m_writeSector] = m_writeBlock;
						storageChanged(true);
					}
					if(m_writeSector + 1 >= g_sectorCount)
						m_singleWrite = true;
					++m_writeSector;
					m_writeState = WriteState::Response;
					m_writeResponseCursor = 0;
					m_dataOutput = 0xf0;
				}
				break;
			case WriteState::Response:
			case WriteState::Idle:
				break;
			}
			return;
		}

		if(m_writeState != WriteState::Response)
			return;

		// Four idle edges, then the five-bit accepted token (00101), a short busy
		// interval, and finally DAT0 high/ready.
		constexpr std::array<bool, 5> accepted{{false, false, true, false, true}};
		if(m_writeResponseCursor < 4)
			m_dataOutput = 0xf0;
		else if(m_writeResponseCursor < 4 + accepted.size())
			m_dataOutput = accepted[m_writeResponseCursor - 4] ? 0xf0 : 0x70;
		else if(m_writeResponseCursor < 4 + accepted.size() + 8)
			m_dataOutput = 0x70;
		else
		{
			m_dataOutput = 0xf0;
			m_writeState = m_singleWrite ? WriteState::Idle : WriteState::AwaitStart;
			m_writeCursor = 0;
			m_writeSawIdleData = false;
			return;
		}
		++m_writeResponseCursor;
	}

	void PlusDrive::queueShortResponse(const uint32_t _value)
	{
		const std::array<uint8_t, 5> response{{
			0x3f,
			static_cast<uint8_t>(_value >> 24),
			static_cast<uint8_t>(_value >> 16),
			static_cast<uint8_t>(_value >> 8),
			static_cast<uint8_t>(_value)}};
		queueResponse(response.data(), response.size());
	}

	void PlusDrive::queueLongResponse(const std::array<uint8_t, 16>& _value)
	{
		std::array<uint8_t, 17> response{};
		response[0] = 0x3f;
		std::copy(_value.begin(), _value.end(), response.begin() + 1);
		queueResponse(response.data(), response.size());
	}

	void PlusDrive::queueResponse(const uint8_t* const _bytes, const size_t _size)
	{
		m_response.fill(0xff);
		std::copy_n(_bytes, std::min(_size, m_response.size()), m_response.begin());
		m_responseBits = std::min(_size, m_response.size()) * 8;
		m_responseCursor = 0;
		// The host supplies two turnaround edges before it starts polling CMD.
		m_responseDelay = 2;
		m_commandOutput = true;
	}
}
