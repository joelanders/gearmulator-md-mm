#include "mdfirmwareupdate.h"

#include "mdtypes.h"

#include <algorithm>
#include <array>

namespace md::firmwareUpdate
{
	namespace
	{
		// The pre-ELE transport and aPLib variant are based on the MIT-licensed
		// elektron-firmware-tool project (Copyright 2026 Marcel Bierling). See
		// elektron-firmware-tool.LICENSE. This implementation is deliberately limited
		// to official MD/MM update files.
		constexpr std::array<uint8_t, 8> g_magic{{'G','S','Y','X','R','O','M','1'}};
		constexpr size_t g_magicOffset = 0x10;
		constexpr size_t g_headerSize = 0x100;
		constexpr size_t g_flashUpdateOffset = 0x4000;
		constexpr size_t g_payloadOffset = 0x200000;
		constexpr size_t g_tableOffset = 0x30;
		constexpr size_t g_recordSize = 12;
		constexpr uint32_t g_formatVersion = 1;
		constexpr size_t g_sectionCount = 5;
		constexpr uint8_t g_deviceMd = 0x02;
		constexpr uint8_t g_deviceMm = 0x03;
		constexpr size_t g_transportHeader = 14;
		constexpr size_t g_sectionHeader = 8;
		constexpr uint32_t g_aplibOffsetBias = 767;
		constexpr uint32_t g_aplibReuseGamma = 2;
		constexpr uint32_t g_aplibFarThreshold = 3328;

		uint32_t readBe32(const uint8_t* const _p)
		{
			return (static_cast<uint32_t>(_p[0]) << 24)
				| (static_cast<uint32_t>(_p[1]) << 16)
				| (static_cast<uint32_t>(_p[2]) << 8) | _p[3];
		}

		void writeBe32(std::vector<uint8_t>& _data, const size_t _offset,
			const uint32_t _value)
		{
			_data[_offset] = static_cast<uint8_t>(_value >> 24);
			_data[_offset + 1] = static_cast<uint8_t>(_value >> 16);
			_data[_offset + 2] = static_cast<uint8_t>(_value >> 8);
			_data[_offset + 3] = static_cast<uint8_t>(_value);
		}

		uint32_t readLe24(const uint8_t* const _p)
		{
			return static_cast<uint32_t>(_p[0])
				| (static_cast<uint32_t>(_p[1]) << 8)
				| (static_cast<uint32_t>(_p[2]) << 16);
		}

		uint32_t decodeNibbles(const uint8_t* const _p)
		{
			uint32_t value = 0;
			for(size_t i = 0; i < 6; ++i)
				value = (value << 4) | _p[i];
			return value;
		}

		uint8_t packetChecksum(const uint8_t _device, const uint8_t* const _nibbles,
			const unsigned _payloadSum)
		{
			unsigned sum = _payloadSum;
			// MD/MM use the non-Octatrack legacy checksum branch.
			if(_device == g_deviceMd || _device == g_deviceMm)
				sum += _nibbles[1] + (_nibbles[2] << 4) + _nibbles[3]
					+ ((_nibbles[4] & 0x0c) << 4);
			return static_cast<uint8_t>(sum);
		}

		bool decodeTransport(const std::vector<uint8_t>& _sysex,
			std::vector<uint8_t>& _container, uint8_t& _device, std::string& _error)
		{
			_container.clear();
			_device = 0;
			bool sawData = false;
			bool sawLength = false;
			uint32_t declaredLength = 0;

			for(size_t begin = 0; begin < _sysex.size();)
			{
				if(_sysex[begin] != 0xf0)
				{
					++begin;
					continue;
				}
				const auto endIt = std::find(_sysex.begin() + static_cast<std::ptrdiff_t>(begin + 1),
					_sysex.end(), uint8_t{0xf7});
				if(endIt == _sysex.end())
				{
					_error = "unterminated SysEx message";
					return false;
				}
				const size_t end = static_cast<size_t>(endIt - _sysex.begin());
				const uint8_t* const body = _sysex.data() + begin + 1;
				const size_t length = end - begin - 1;
				begin = end + 1;

				if(length < 6 || body[0] != 0x00 || body[1] != 0x20 || body[2] != 0x3c)
					continue;
				if(body[3] != g_deviceMd && body[3] != g_deviceMm)
					continue;
				if(_device && _device != body[3])
				{
					_error = "mixed Machinedrum and Monomachine SysEx messages";
					return false;
				}
				_device = body[3];

				if(body[5] == 0x7e && length > g_transportHeader)
				{
					for(size_t i = 6; i < g_transportHeader; ++i)
					{
						if(body[i] > 0x0f)
						{
							_error = "invalid legacy packet counter/checksum nibble";
							return false;
						}
					}
					const size_t encodedLength = length - g_transportHeader;
					if(encodedLength % 3)
					{
						_error = "truncated 2+7+7 firmware packet";
						return false;
					}
					const uint32_t counter = decodeNibbles(body + 8);
					if(counter != 0x4000u + _container.size())
					{
						_error = "out-of-order or missing firmware packet";
						return false;
					}

					std::vector<uint8_t> decoded;
					decoded.reserve(encodedLength / 3 * 2);
					for(size_t i = g_transportHeader; i < length; i += 3)
					{
						if(body[i] > 3 || body[i + 1] > 0x7f || body[i + 2] > 0x7f)
						{
							_error = "invalid 2+7+7 firmware payload";
							return false;
						}
						const uint16_t word = static_cast<uint16_t>((body[i] & 3) << 14)
							| static_cast<uint16_t>(body[i + 1] << 7) | body[i + 2];
						decoded.push_back(static_cast<uint8_t>(word >> 8));
						decoded.push_back(static_cast<uint8_t>(word));
					}
					unsigned sum = 0;
					for(const auto byte : decoded)
						sum += byte;
					const uint8_t checksum = packetChecksum(_device, body + 8, sum);
					if(body[6] != ((checksum >> 4) & 0x0f) || body[7] != (checksum & 0x0f))
					{
						_error = "firmware packet checksum mismatch";
						return false;
					}
					_container.insert(_container.end(), decoded.begin(), decoded.end());
					sawData = true;
				}
				else if(body[5] == 0x7f && length >= 12)
				{
					for(size_t i = 6; i < 12; ++i)
					{
						if(body[i] > 0x0f)
						{
							_error = "invalid firmware length marker";
							return false;
						}
					}
					declaredLength = decodeNibbles(body + 6);
					sawLength = true;
				}
			}

			if(!sawData || !_device)
			{
				_error = "not a Machinedrum or Monomachine OS update SysEx";
				return false;
			}
			if(!sawLength || declaredLength > _container.size()
				|| _container.size() - declaredLength > 1)
			{
				_error = "firmware length marker is missing or inconsistent";
				return false;
			}
			_container.resize(declaredLength);
			return true;
		}

		class BitReader
		{
		public:
			BitReader(const uint8_t* const _begin, const uint8_t* const _end)
				: m_current(_begin), m_end(_end) {}

			bool bit(uint32_t& _value)
			{
				m_tag <<= 1;
				if((m_tag & 0xff) == 0)
				{
					if(m_current == m_end)
						return false;
					const uint32_t byte = *m_current++;
					m_tag = (byte << 1) | 1;
					_value = (byte >> 7) & 1;
				}
				else
					_value = (m_tag >> 8) & 1;
				return true;
			}

			bool byte(uint32_t& _value)
			{
				if(m_current == m_end)
					return false;
				_value = *m_current++;
				return true;
			}

			bool gamma(uint32_t& _value)
			{
				_value = 1;
				for(;;)
				{
					uint32_t data = 0;
					uint32_t last = 0;
					if(!bit(data) || _value > 0x02000000u)
						return false;
					_value = (_value << 1) + data;
					if(!bit(last))
						return false;
					if(last)
						return true;
				}
			}

		private:
			const uint8_t* m_current;
			const uint8_t* const m_end;
			uint32_t m_tag = 0;
		};

		bool decompressSection(const uint8_t* const _source, const size_t _length,
			const size_t _limit, std::vector<uint8_t>& _output, std::string& _error)
		{
			if(_length < g_sectionHeader)
				return false;
			BitReader bits(_source + g_sectionHeader, _source + _length);
			_output.clear();
			_output.reserve(std::min<size_t>(_limit, _length * 4));
			uint32_t lastOffset = 1;
			for(;;)
			{
				uint32_t literal = 0;
				if(!bits.bit(literal))
					break;
				if(literal)
				{
					uint32_t value = 0;
					if(!bits.byte(value) || _output.size() == _limit)
						break;
					_output.push_back(static_cast<uint8_t>(value));
					continue;
				}

				uint32_t gamma = 0;
				uint32_t offset = 0;
				if(!bits.gamma(gamma))
					break;
				if(gamma == g_aplibReuseGamma)
					offset = lastOffset;
				else
				{
					uint32_t low = 0;
					if(!bits.byte(low))
						break;
					const uint32_t rawOffset = (gamma << 8) + low;
					if(rawOffset == g_aplibOffsetBias)
						return !_output.empty();
					if(rawOffset < g_aplibOffsetBias)
						break;
					offset = rawOffset - g_aplibOffsetBias;
					lastOffset = offset;
				}

				uint32_t a = 0;
				uint32_t b = 0;
				if(!bits.bit(a) || !bits.bit(b))
					break;
				uint32_t length = 2 * a + b;
				if(!length)
				{
					if(!bits.gamma(length))
						break;
					length += 2;
				}
				if(offset > g_aplibFarThreshold)
					++length;
				++length;
				if(!offset || offset > _output.size() || length > _limit - _output.size())
					break;
				for(uint32_t i = 0; i < length; ++i)
					_output.push_back(_output[_output.size() - offset]);
			}
			_error = "invalid or truncated aPLib firmware section";
			return false;
		}

		bool unpackSections(const std::vector<uint8_t>& _container, const uint8_t _device,
			std::array<std::vector<uint8_t>, g_sectionCount>& _sections,
			std::array<uint32_t, g_sectionCount>& _compressedOffsets, std::string& _error)
		{
			size_t offset = 0;
			for(size_t section = 0; section < g_sectionCount; ++section)
			{
				bool found = false;
				while(offset + g_sectionHeader <= _container.size())
				{
					const uint32_t compressedSize = readBe32(_container.data() + offset);
					const size_t end = offset + g_sectionHeader + compressedSize;
					if(compressedSize >= 16 && end <= _container.size())
					{
						uint32_t sum = 0;
						for(size_t i = offset + g_sectionHeader; i < end; ++i)
							sum += _container[i];
						if(sum == readBe32(_container.data() + offset + 4))
						{
							const size_t limit = section == static_cast<size_t>(Section::MainOs)
								|| section == static_cast<size_t>(Section::FactoryData)
								? 0x100000u : 0x200000u;
							if(!decompressSection(_container.data() + offset,
								g_sectionHeader + compressedSize, limit, _sections[section], _error))
								return false;
							_compressedOffsets[section] = static_cast<uint32_t>(offset);
							offset = end;
							found = true;
							break;
						}
					}
					++offset;
				}
				if(!found)
				{
					_error = "missing or corrupt firmware section " + std::to_string(section);
					return false;
				}
			}
			if(_sections[0].empty() || _sections[0].size() > 0x100000
				|| _sections[4].size() > 0x100000)
			{
				_error = "firmware payload does not fit the emulated memory map";
				return false;
			}
			const size_t dspSectionCount = _device == g_deviceMd ? 2 : 3;
			for(size_t i = 1; i <= dspSectionCount; ++i)
			{
				if(_sections[i].empty() || _sections[i].size() % 3)
				{
					_error = "invalid 24-bit DSP firmware section";
					return false;
				}
			}
			uint32_t mixerEntry = 0;
			uint32_t producerEntry = 0;
			if(!visitDspCommands(_sections[1], nullptr, nullptr, mixerEntry, _error)
				|| !visitDspCommands(_sections[2], nullptr, nullptr, producerEntry, _error))
				return false;
			if(mixerEntry != producerEntry)
			{
				_error = "DSP firmware sections disagree on their entry point";
				return false;
			}
			if(_device == g_deviceMd)
			{
				// Machinedrum carries two complete DSP command streams followed by two
				// data images. Normalize the private Gearmulator section table to the
				// mixer/producer/shared shape used by the boot path. The physical
				// updater order is producer first, mixer second; the untouched
				// compressed third data image remains available in the flash copy.
				if(_sections[3].empty() || _sections[3].size() > 0x100000)
				{
					_error = "invalid Machinedrum data section";
					return false;
				}
				std::swap(_sections[1], _sections[2]);
				// The first data section remains compressed in flash and is the
				// handoff pointer. The second expands into battery-backed patch RAM.
				_compressedOffsets[4] = _compressedOffsets[3];
				_sections[3].clear();
			}
			else
			{
				uint32_t commonEntry = 0;
				if(!visitDspCommands(_sections[3], nullptr, nullptr, commonEntry, _error))
					return false;
				if(mixerEntry != commonEntry)
				{
					_error = "DSP firmware sections disagree on their entry point";
					return false;
				}
			}
			return true;
		}

		bool locateRecord(const std::vector<uint8_t>& _rom, const Section _section,
			uint32_t& _offset, uint32_t& _length)
		{
			if(!isConvertedRom(_rom))
				return false;
			const uint32_t wanted = static_cast<uint32_t>(_section);
			for(size_t i = 0; i < g_sectionCount; ++i)
			{
				const size_t record = g_tableOffset + i * g_recordSize;
				if(readBe32(_rom.data() + record) != wanted)
					continue;
				_offset = readBe32(_rom.data() + record + 4);
				_length = readBe32(_rom.data() + record + 8);
				return _offset >= g_headerSize && _offset <= _rom.size()
					&& _length <= _rom.size() - _offset;
			}
			return false;
		}
	}

	bool convertSysexToRom(const std::vector<uint8_t>& _sysex,
		std::vector<uint8_t>& _rom, MachineModel& _model, std::string& _error)
	{
		_rom.clear();
		_error.clear();
		std::vector<uint8_t> container;
		uint8_t device = 0;
		if(!decodeTransport(_sysex, container, device, _error))
			return false;
		_model = device == g_deviceMm ? MachineModel::Monomachine : MachineModel::Machinedrum;

		std::array<std::vector<uint8_t>, g_sectionCount> sections;
		std::array<uint32_t, g_sectionCount> compressedOffsets{};
		if(!unpackSections(container, device, sections, compressedOffsets, _error))
			return false;

		_rom.assign(g_romSize, 0xff);
		// Reset directly into the already-decompressed main OS. Bytes 8..11 retain
		// the existing model discriminator used by RomLoader.
		writeBe32(_rom, 0, 0x00300000);
		writeBe32(_rom, 4, 0x00200000);
		writeBe32(_rom, 8, _model == MachineModel::Monomachine
			? 0x00000248 : 0x6000047a);
		std::copy(g_magic.begin(), g_magic.end(), _rom.begin() + g_magicOffset);
		writeBe32(_rom, 0x18, g_formatVersion);
		writeBe32(_rom, 0x1c, device);
		writeBe32(_rom, 0x20, static_cast<uint32_t>(g_sectionCount));
		writeBe32(_rom, 0x24, static_cast<uint32_t>(g_flashUpdateOffset)
			+ compressedOffsets[static_cast<size_t>(Section::FactoryData)]);
		// Official pre-ELE update containers are the byte-exact flash payload at
		// 0x4000. Preserve that mapping for OS code which reads its own flash.
		if(container.size() > g_payloadOffset - g_flashUpdateOffset)
		{
			_rom.clear();
			_error = "firmware flash payload overlaps the Gearmulator section area";
			return false;
		}
		std::copy(container.begin(), container.end(),
			_rom.begin() + g_flashUpdateOffset);

		size_t payloadOffset = g_payloadOffset;
		for(size_t i = 0; i < g_sectionCount; ++i)
		{
			payloadOffset = (payloadOffset + 15) & ~size_t{15};
			if(sections[i].size() > _rom.size() - payloadOffset)
			{
				_rom.clear();
				_error = "decoded firmware does not fit the Gearmulator update image";
				return false;
			}
			const size_t record = g_tableOffset + i * g_recordSize;
			writeBe32(_rom, record, static_cast<uint32_t>(i));
			writeBe32(_rom, record + 4, static_cast<uint32_t>(payloadOffset));
			writeBe32(_rom, record + 8, static_cast<uint32_t>(sections[i].size()));
			std::copy(sections[i].begin(), sections[i].end(), _rom.begin() + payloadOffset);
			payloadOffset += sections[i].size();
		}
		return true;
	}

	bool isConvertedRom(const std::vector<uint8_t>& _rom)
	{
		return _rom.size() == g_romSize
			&& std::equal(g_magic.begin(), g_magic.end(), _rom.begin() + g_magicOffset)
			&& readBe32(_rom.data() + 0x18) == g_formatVersion
			&& readBe32(_rom.data() + 0x20) == g_sectionCount;
	}

	bool model(const std::vector<uint8_t>& _rom, MachineModel& _model)
	{
		if(!isConvertedRom(_rom))
			return false;
		const uint32_t device = readBe32(_rom.data() + 0x1c);
		if(device == g_deviceMd)
			_model = MachineModel::Machinedrum;
		else if(device == g_deviceMm)
			_model = MachineModel::Monomachine;
		else
			return false;
		return true;
	}

	bool factoryFlashAddress(const std::vector<uint8_t>& _rom, uint32_t& _address)
	{
		if(!isConvertedRom(_rom))
			return false;
		_address = readBe32(_rom.data() + 0x24);
		return _address >= g_flashUpdateOffset && _address < g_payloadOffset;
	}

	bool readSection(const std::vector<uint8_t>& _rom, const Section _section,
		std::vector<uint8_t>& _data)
	{
		uint32_t offset = 0;
		uint32_t length = 0;
		if(!locateRecord(_rom, _section, offset, length))
			return false;
		_data.assign(_rom.begin() + offset, _rom.begin() + offset + length);
		return true;
	}

	bool visitDspCommands(const std::vector<uint8_t>& _section, const DspWrite _write,
		void* const _context, uint32_t& _entry, std::string& _error)
	{
		if(_section.size() < 12 || _section.size() % 3)
		{
			_error = "DSP section is not a 24-bit command stream";
			return false;
		}
		const size_t wordCount = _section.size() / 3;
		if(readLe24(_section.data()) != 3)
		{
			_error = "DSP section has no execute descriptor";
			return false;
		}
		_entry = readLe24(_section.data() + 3);
		size_t cursor = 2;
		while(cursor < wordCount)
		{
			const uint32_t command = readLe24(_section.data() + cursor * 3);
			if(command == 4)
			{
				// Legacy Machinedrum images place one zero-valued stream selector
				// immediately after the leading execute descriptor. It is metadata
				// for the ColdFire-side sender, not a DSP memory command.
				if(cursor != 2 || cursor + 2 > wordCount
					|| readLe24(_section.data() + (cursor + 1) * 3) != 0)
				{
					_error = "invalid DSP stream selector";
					return false;
				}
				cursor += 2;
				continue;
			}
			if(command == 3)
			{
				if(cursor + 2 != wordCount)
				{
					_error = "DSP execute command is not the final record";
					return false;
				}
				const uint32_t tailEntry = readLe24(_section.data() + (cursor + 1) * 3);
				if(tailEntry != _entry)
				{
					_error = "DSP execute descriptors disagree";
					return false;
				}
				return true;
			}
			if(command > 2 || cursor + 3 > wordCount)
			{
				_error = "unknown or truncated DSP memory command";
				return false;
			}
			const uint32_t address = readLe24(_section.data() + (cursor + 1) * 3);
			const uint32_t count = readLe24(_section.data() + (cursor + 2) * 3);
			cursor += 3;
			if(count > wordCount - cursor)
			{
				_error = "truncated DSP memory record";
				return false;
			}
			if(_write)
			{
				std::vector<uint32_t> words;
				words.reserve(count);
				for(uint32_t i = 0; i < count; ++i)
					words.push_back(readLe24(_section.data() + (cursor + i) * 3));
				if(!_write(_context, command, address, words.data(), words.size()))
				{
					_error = "DSP memory record is outside the emulated address space";
					return false;
				}
			}
			cursor += count;
		}
		_error = "DSP section has no final execute command";
		return false;
	}
}
