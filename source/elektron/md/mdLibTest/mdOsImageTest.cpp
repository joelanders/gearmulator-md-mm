#include "mdLib/mdosimage.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	std::vector<uint8_t> encodeMessage(const uint32_t _address,
		const std::vector<uint8_t>& _bytes, const uint8_t _product)
	{
		require(_bytes.size() % 2 == 0, "odd test payload");
		std::vector<uint8_t> msg{
			0xf0, 0x00, 0x20, 0x3c, _product, 0x00, 0x7e,
			0x00, 0x00, 0x00,
			static_cast<uint8_t>((_address >> 16) & 0x0f),
			static_cast<uint8_t>((_address >> 12) & 0x0f),
			static_cast<uint8_t>((_address >> 8) & 0x0f),
			static_cast<uint8_t>((_address >> 4) & 0x0f),
			0x00};
		for(size_t i = 0; i < _bytes.size(); i += 2)
		{
			const uint32_t word = (static_cast<uint32_t>(_bytes[i]) << 8)
				| _bytes[i + 1];
			msg.push_back(static_cast<uint8_t>((word >> 14) & 0x03));
			msg.push_back(static_cast<uint8_t>((word >> 7) & 0x7f));
			msg.push_back(static_cast<uint8_t>(word & 0x7f));
		}
		msg.push_back(0xf7);
		return msg;
	}

	void syntheticRoundTrip()
	{
		// Independent encoder: random payloads must survive a decode trip.
		uint32_t state = 0x12345678;
		auto next = [&]()
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return state;
		};
		std::vector<uint8_t> file;
		uint32_t address = 0x4000;
		std::vector<std::pair<uint32_t, std::vector<uint8_t>>> expected;
		for(int m = 0; m < 40; ++m)
		{
			std::vector<uint8_t> payload(64);
			for(auto& b : payload)
				b = static_cast<uint8_t>(next() >> (next() & 31));
			const auto msg = encodeMessage(address, payload, 0x02);
			file.insert(file.end(), msg.begin(), msg.end());
			expected.emplace_back(address, payload);
			address += static_cast<uint32_t>(payload.size());
		}
		file.push_back(0xf0);
		const uint8_t tail[] = {0x00, 0x20, 0x3c, 0x02, 0x00, 0x7f, 0x00, 0xf7};
		file.insert(file.end(), std::begin(tail), std::end(tail));

		md::OsUpgradeDecodeResult result;
		require(md::decodeOsUpgradeSysex(result, file),
			"synthetic file did not decode");
		require(result.model == md::MachineModel::Machinedrum, "wrong model");
		require(result.messages.size() == expected.size(), "message count");
		require(result.hasVerifyCommand, "verify command lost");
		for(size_t i = 0; i < expected.size(); ++i)
		{
			require(result.messages[i].address == expected[i].first,
				"address mismatch");
			require(result.messages[i].data == expected[i].second,
				"payload mismatch");
		}
	}

	void syntheticFailures()
	{
		std::vector<uint8_t> payload(64, 0x55);
		const auto good = encodeMessage(0x4000, payload, 0x02);
		md::OsUpgradeDecodeResult result;

		// Truncated message.
		require(!md::decodeOsUpgradeSysex(result,
			std::vector<uint8_t>(good.begin(), good.end() - 1)),
			"truncation accepted");
		// Corrupt address nibble.
		auto badNibble = good;
		badNibble[10] = 0x40;
		require(!md::decodeOsUpgradeSysex(result, badNibble),
			"bad nibble accepted");
		// Out-of-range datum.
		auto badDatum = good;
		badDatum[16] = 0x80;
		require(!md::decodeOsUpgradeSysex(result, badDatum),
			"8-bit datum accepted");
		// Address gap between messages.
		auto gap = good;
		const auto second = encodeMessage(0x4000 + 128, payload, 0x02);
		gap.insert(gap.end(), second.begin(), second.end());
		require(!md::decodeOsUpgradeSysex(result, gap), "gap accepted");
		// Mixed product ids.
		auto mixed = good;
		const auto mm = encodeMessage(0x4040, payload, 0x03);
		mixed.insert(mixed.end(), mm.begin(), mm.end());
		require(!md::decodeOsUpgradeSysex(result, mixed),
			"mixed models accepted");
		// Pure framing file.
		const std::vector<uint8_t> framing{
			0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x7e, 0xf7};
		require(!md::decodeOsUpgradeSysex(result, framing),
			"framing-only accepted");
	}

	std::vector<std::string> listSyx(const std::string& _dir)
	{
		std::vector<std::string> files;
		for(const auto& sub : {"", "/official-firmware-md-mm",
			"/custom-firmware-md-mm"})
		{
			std::vector<std::string> found;
			if(baseLib::filesystem::findFiles(found, _dir + sub, ".syx", 1,
				1u << 31))
				files.insert(files.end(), found.begin(), found.end());
		}
		std::sort(files.begin(), files.end());
		return files;
	}

	std::vector<uint8_t> assemble(const md::OsUpgradeDecodeResult& _result,
		const std::vector<uint8_t>& _rom)
	{
		std::vector<uint8_t> image = _rom;
		for(const auto& message : _result.messages)
		{
			require(message.address + message.data.size() <= image.size(),
				"message outside image");
			std::copy(message.data.begin(), message.data.end(),
				image.begin() + message.address);
		}
		return image;
	}

	std::vector<uint8_t> readRom(const char* const _env, const md::MachineModel _model)
	{
		const auto* const path = std::getenv(_env);
		if(!path || !*path)
			return {};
		std::vector<uint8_t> rom;
		if(!baseLib::filesystem::readFile(rom, path)
			|| !md::RomLoader::isRomForModel(rom, _model))
			return {};
		return rom;
	}

	void fileBackedTests()
	{
		const auto* const dir = std::getenv("GEARMULATOR_MDMM_SYSEX_DIR");
		if(!dir || !*dir)
		{
			std::cout << "mdOsImageTest: SKIP (GEARMULATOR_MDMM_SYSEX_DIR not supplied)\n";
			return;
		}
		const auto files = listSyx(dir);
		require(!files.empty(), "no .syx files found");

		const auto mdRom = readRom("GEARMULATOR_MD_FIRMWARE_BIN",
			md::MachineModel::Machinedrum);
		const auto mmRom = readRom("GEARMULATOR_MM_FIRMWARE_BIN",
			md::MachineModel::Monomachine);

		size_t verified = 0;
		for(const auto& path : files)
		{
			std::vector<uint8_t> bytes;
			require(baseLib::filesystem::readFile(bytes, path),
				"could not read sysex file");
			md::OsUpgradeDecodeResult result;
			require(md::decodeOsUpgradeSysex(result, bytes),
				("decode failed: " + path).c_str());
			require(!result.messages.empty(), "no data messages");
			require(result.hasVerifyCommand, "verify command missing");

			const bool isMD = result.model == md::MachineModel::Machinedrum;
			const auto& rom = isMD ? mdRom : mmRom;
			const auto name = path.substr(path.find_last_of("/\\") + 1);
			if(!rom.empty())
			{
				const auto image = assemble(result, rom);
				for(const auto& message : result.messages)
				{
					require(std::equal(message.data.begin(), message.data.end(),
						image.begin() + message.address), "assembly mismatch");
				}
				// Official OS files must reproduce their stock ROM exactly
				// over the covered range.
				if(name.find("1.63") != std::string::npos
					|| name.find("1.32") != std::string::npos)
				{
					require(std::equal(image.begin() + result.baseAddress,
						image.begin() + result.endAddress,
						rom.begin() + result.baseAddress),
						("official image differs from ROM: " + name).c_str());
					++verified;
				}
			}
			std::cout << "decoded " << name << ": "
				<< (isMD ? "MD" : "MM") << ' ' << result.messages.size()
				<< " msgs [" << std::hex << result.baseAddress
				<< '-' << result.endAddress << std::dec << "]\n";
		}
		if(!mdRom.empty() || !mmRom.empty())
			require(verified >= 1, "no official round trip verified");
	}
}

int main()
{
	try
	{
		syntheticRoundTrip();
		syntheticFailures();
		fileBackedTests();
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
	std::cout << "mdOsImageTest passed\n";
	return 0;
}
