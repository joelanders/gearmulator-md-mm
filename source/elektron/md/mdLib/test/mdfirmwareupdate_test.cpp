#include "mdLib/mdfirmwareupdate.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	void appendWord(std::vector<uint8_t>& _data, const uint32_t _word)
	{
		_data.push_back(static_cast<uint8_t>(_word));
		_data.push_back(static_cast<uint8_t>(_word >> 8));
		_data.push_back(static_cast<uint8_t>(_word >> 16));
	}

	void writeBe32(std::vector<uint8_t>& _data, const size_t _offset,
		const uint32_t _value)
	{
		_data[_offset] = static_cast<uint8_t>(_value >> 24);
		_data[_offset + 1] = static_cast<uint8_t>(_value >> 16);
		_data[_offset + 2] = static_cast<uint8_t>(_value >> 8);
		_data[_offset + 3] = static_cast<uint8_t>(_value);
	}

	class LiteralPacker
	{
	public:
		LiteralPacker() : m_data(8, 0) {}

		void bit(const bool _value)
		{
			if(!m_tagBits)
			{
				m_tagPosition = m_data.size();
				m_data.push_back(0);
				m_tagBits = 8;
			}
			if(_value)
				m_data[m_tagPosition] |= static_cast<uint8_t>(1u << (m_tagBits - 1));
			--m_tagBits;
		}

		void byte(const uint8_t _value) { m_data.push_back(_value); }

		void gamma(const uint32_t _value)
		{
			unsigned bits = 0;
			for(uint32_t value = _value; value; value >>= 1)
				++bits;
			for(int bitIndex = static_cast<int>(bits) - 2; bitIndex >= 0; --bitIndex)
			{
				bit((_value >> bitIndex) & 1);
				bit(bitIndex == 0);
			}
		}

		std::vector<uint8_t> finish(const std::vector<uint8_t>& _input)
		{
			for(const auto value : _input)
			{
				bit(true);
				byte(value);
			}
			// The Elektron aPLib variant terminates with an intentionally wrapping
			// raw offset: (0x1000002 << 8) + 0xff == 767 in 32 bits.
			bit(false);
			gamma(0x01000002);
			byte(0xff);
			writeBe32(m_data, 0, static_cast<uint32_t>(m_data.size() - 8));
			uint32_t sum = 0;
			for(size_t i = 8; i < m_data.size(); ++i)
				sum += m_data[i];
			writeBe32(m_data, 4, sum);
			return std::move(m_data);
		}

	private:
		std::vector<uint8_t> m_data;
		size_t m_tagPosition = 0;
		unsigned m_tagBits = 0;
	};

	std::vector<uint8_t> pack(const std::vector<uint8_t>& _input)
	{
		return LiteralPacker{}.finish(_input);
	}

	void appendNibbles(std::vector<uint8_t>& _data, const uint32_t _value)
	{
		for(int shift = 20; shift >= 0; shift -= 4)
			_data.push_back(static_cast<uint8_t>((_value >> shift) & 0x0f));
	}

	std::vector<uint8_t> makeSysex(const uint8_t _device,
		const std::vector<std::vector<uint8_t>>& _sections)
	{
		std::vector<uint8_t> container;
		for(const auto& section : _sections)
		{
			const auto compressed = pack(section);
			container.insert(container.end(), compressed.begin(), compressed.end());
		}
		std::vector<uint8_t> result{0xf0, 0x00, 0x20, 0x3c, _device, 0x00, 0x7e};
		const size_t checksumPosition = result.size();
		result.push_back(0);
		result.push_back(0);
		std::vector<uint8_t> counter;
		appendNibbles(counter, 0x4000);
		result.insert(result.end(), counter.begin(), counter.end());
		unsigned payloadSum = 0;
		for(size_t i = 0; i < container.size(); i += 2)
		{
			const uint16_t word = static_cast<uint16_t>(container[i] << 8)
				| (i + 1 < container.size() ? container[i + 1] : 0);
			result.push_back(static_cast<uint8_t>(word >> 14));
			result.push_back(static_cast<uint8_t>((word >> 7) & 0x7f));
			result.push_back(static_cast<uint8_t>(word & 0x7f));
			payloadSum += word >> 8;
			payloadSum += word & 0xff;
		}
		const unsigned checksum = payloadSum + counter[1] + (counter[2] << 4)
			+ counter[3] + ((counter[4] & 0x0c) << 4);
		result[checksumPosition] = static_cast<uint8_t>((checksum >> 4) & 0x0f);
		result[checksumPosition + 1] = static_cast<uint8_t>(checksum & 0x0f);
		result.push_back(0xf7);
		result.insert(result.end(), {0xf0, 0x00, 0x20, 0x3c, _device, 0x00, 0x7f});
		appendNibbles(result, static_cast<uint32_t>(container.size()));
		result.push_back(0xf7);
		return result;
	}

	struct Capture
	{
		uint32_t space = 0;
		uint32_t address = 0;
		std::vector<uint32_t> words;
	};

	bool capture(void* const _context, const uint32_t _space, const uint32_t _address,
		const uint32_t* const _words, const size_t _count)
	{
		auto& result = *static_cast<Capture*>(_context);
		result.space = _space;
		result.address = _address;
		result.words.assign(_words, _words + _count);
		return true;
	}

	bool checkSyntheticConversion(const uint8_t _device,
		const md::MachineModel _expectedModel)
	{
		std::vector<uint8_t> dsp;
		for(const uint32_t word : {3u, 0x66u, 0u, 0x10u, 2u,
			0x123456u, 0xabcdefu, 3u, 0x66u})
			appendWord(dsp, word);
		const std::vector<std::vector<uint8_t>> sections{
			std::vector<uint8_t>(32, 0x4e), dsp, dsp, dsp,
			std::vector<uint8_t>(32, 0xa5)
		};
		auto sysex = makeSysex(_device, sections);
		std::vector<uint8_t> rom;
		md::MachineModel model = md::MachineModel::Machinedrum;
		std::string error;
		if(!md::firmwareUpdate::convertSysexToRom(sysex, rom, model, error)
			|| model != _expectedModel || !md::firmwareUpdate::isConvertedRom(rom)
			|| rom.size() != md::g_romSize)
		{
			std::fprintf(stderr, "synthetic conversion failed: %s\n", error.c_str());
			return false;
		}
		for(size_t i = 0; i < sections.size(); ++i)
		{
			std::vector<uint8_t> decoded;
			if(!md::firmwareUpdate::readSection(rom,
				static_cast<md::firmwareUpdate::Section>(i), decoded) || decoded != sections[i])
			{
				std::fprintf(stderr, "synthetic section %zu did not round-trip\n", i);
				return false;
			}
		}
		sysex[7] ^= 1;
		if(md::firmwareUpdate::convertSysexToRom(sysex, rom, model, error))
		{
			std::fputs("corrupt packet checksum was accepted\n", stderr);
			return false;
		}
		return true;
	}

	std::vector<uint8_t> load(const char* const _path)
	{
		std::ifstream input(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	void writeLcdPgm(const md::Hardware& _hardware)
	{
		const auto* const path = std::getenv("MD_FIRMWARE_UPDATE_LCD_PGM");
		if(!path || !*path)
			return;
		const auto panel = _hardware.getFrontPanelSnapshot();
		std::ofstream output(path, std::ios::binary);
		output << "P5\n" << md::FrontPanel::g_lcdWidth << ' '
			<< md::FrontPanel::g_lcdHeight << "\n255\n";
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
		{
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				output.put(panel.getLcdPixel(x, y) ? '\0' : '\xff');
		}
	}
}

int main(const int _argc, char** const _argv)
{
	std::vector<uint8_t> stream;
	for(const uint32_t word : {3u, 0x66u, 0u, 0x10u, 2u,
		0x123456u, 0xabcdefu, 3u, 0x66u})
		appendWord(stream, word);
	Capture captured;
	uint32_t entry = 0;
	std::string error;
	if(!md::firmwareUpdate::visitDspCommands(stream, capture, &captured,
		entry, error) || entry != 0x66 || captured.space != 0
		|| captured.address != 0x10
		|| captured.words != std::vector<uint32_t>({0x123456u, 0xabcdefu})
		|| !checkSyntheticConversion(0x02, md::MachineModel::Machinedrum)
		|| !checkSyntheticConversion(0x03, md::MachineModel::Monomachine))
	{
		std::fprintf(stderr, "mdfirmwareupdate_test: failed: %s\n", error.c_str());
		return 1;
	}

	if(_argc == 1)
	{
		std::puts("mdfirmwareupdate_test: PASS");
		return 0;
	}
	if(_argc != 2)
	{
		std::fprintf(stderr, "usage: mdfirmwareupdate_test [official-os-update.syx]\n");
		return 2;
	}

	const auto input = load(_argv[1]);
	std::vector<uint8_t> rom;
	md::MachineModel model = md::MachineModel::Machinedrum;
	if(input.size() == md::g_romSize)
	{
		rom = input;
		model = md::RomLoader::isRomForModel(rom, md::MachineModel::Monomachine)
			? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
	}
	else if(!md::firmwareUpdate::convertSysexToRom(input, rom, model, error))
	{
		std::fprintf(stderr, "conversion failed: %s\n", error.c_str());
		return 1;
	}
	const bool converted = md::firmwareUpdate::isConvertedRom(rom);
	if(converted)
	{
		if(const auto* const path = std::getenv("MD_FIRMWARE_UPDATE_MAIN_DUMP"))
		{
			std::vector<uint8_t> mainOs;
			if(*path && md::firmwareUpdate::readSection(rom,
				md::firmwareUpdate::Section::MainOs, mainOs))
			{
				std::ofstream output(path, std::ios::binary);
				output.write(reinterpret_cast<const char*>(mainOs.data()),
					static_cast<std::streamsize>(mainOs.size()));
			}
		}
	}
	md::Hardware hardware(rom, _argv[1], model);
	if(!hardware.isValid())
	{
		std::fputs("direct boot initialization failed\n", stderr);
		return 1;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while(hardware.getFrontPanelSnapshot().countLitPixels() <= 2000)
	{
		hardware.advance(64);
		if(std::chrono::steady_clock::now() >= deadline)
		{
			std::fprintf(stderr, "boot did not reach the front panel: uc=%08x\n",
				hardware.getUC().getPC());
			return 1;
		}
	}
	writeLcdPgm(hardware);
	std::printf("mdfirmwareupdate_test: PASS (%s, %zu-byte Gearmulator image)\n",
		model == md::MachineModel::Monomachine ? "Monomachine" : "Machinedrum",
		rom.size());
	return 0;
}
