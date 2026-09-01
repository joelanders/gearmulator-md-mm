#include "mdLib/mdflash.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdfirmwareupdate.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmmwaveforms.h"
#include "mdLib/mdplusdrive.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdsysextransfer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
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
		const bool machinedrum = _device == 0x02;
		auto deviceDsp = dsp;
		if(machinedrum)
		{
			std::vector<uint8_t> selector;
			appendWord(selector, 4);
			appendWord(selector, 0);
			deviceDsp.insert(deviceDsp.begin() + 6, selector.begin(), selector.end());
		}
		auto secondDsp = deviceDsp;
		const size_t firstPayloadByte = (machinedrum ? 7u : 5u) * 3u;
		secondDsp[firstPayloadByte] ^= 1;
		const std::vector<std::vector<uint8_t>> sections{
			std::vector<uint8_t>(32, 0x4e), deviceDsp, secondDsp,
			machinedrum ? std::vector<uint8_t>(32, 0x33) : dsp,
			std::vector<uint8_t>(32, 0xa5)
		};
		auto expectedSections = sections;
		if(machinedrum)
		{
			std::swap(expectedSections[1], expectedSections[2]);
			expectedSections[3].clear();
		}
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
		uint32_t factoryAddress = 0;
		size_t expectedFactoryAddress = 0x4000;
		const size_t handoffSection = machinedrum ? 3 : 4;
		for(size_t i = 0; i < handoffSection; ++i)
			expectedFactoryAddress += pack(sections[i]).size();
		if(!md::firmwareUpdate::factoryFlashAddress(rom, factoryAddress)
			|| factoryAddress != expectedFactoryAddress)
		{
			std::fputs("synthetic factory handoff address is incorrect\n", stderr);
			return false;
		}
		for(size_t i = 0; i < expectedSections.size(); ++i)
		{
			std::vector<uint8_t> decoded;
			if(!md::firmwareUpdate::readSection(rom,
				static_cast<md::firmwareUpdate::Section>(i), decoded)
				|| decoded != expectedSections[i])
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

	bool checkFlashCommands()
	{
		constexpr uint32_t unlock1 = 0x0000aaaa;
		constexpr uint32_t unlock2 = 0x00005554;
		md::FlashCommandDecoder flash;
		flash.write16(unlock1, 0xaaaa);
		flash.write16(unlock2, 0x5555);
		flash.write16(unlock1, 0x9090);
		if(flash.read16(0) != md::FlashCommandDecoder::g_manufacturerId
			|| flash.read16(2) != md::FlashCommandDecoder::g_deviceId)
			return false;
		flash.write16(0, 0xf0f0);

		flash.write16(unlock1, 0xaaaa);
		flash.write16(unlock2, 0x5555);
		flash.write16(unlock1, 0xa0a0);
		const auto program = flash.write16(0x1234, 0xf0f0);
		if(!program || program->type != md::FlashCommandDecoder::Operation::Type::ProgramWord
			|| program->offset != 0x1234 || program->value != 0xf0f0)
			return false;

		flash.write16(unlock1, 0xaaaa);
		flash.write16(unlock2, 0x5555);
		flash.write16(unlock1, 0x8080);
		flash.write16(unlock1, 0xaaaa);
		flash.write16(unlock2, 0x5555);
		const auto erase = flash.write16(0x23456, 0x3030);
		return erase
			&& erase->type == md::FlashCommandDecoder::Operation::Type::EraseSector
			&& erase->offset == 0x23456;
	}

	std::vector<uint8_t> load(const char* const _path)
	{
		std::ifstream input(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	uint64_t fingerprintDigiProBank(md::Hardware& _hardware)
	{
		auto& memory = _hardware.getDspProducer().dsp().memory();
		uint64_t result = 14695981039346656037ull;
		for(uint32_t wave = 0; wave < md::mmwaveforms::g_waveformCount; ++wave)
		{
			const auto base = md::mmwaveforms::g_destinationBase
				+ wave * md::mmwaveforms::g_destinationStride;
			for(uint32_t word = 0; word < md::mmwaveforms::g_wordsPerWave; ++word)
			{
				const auto value = memory.get(dsp56k::MemArea_X, base + word);
				for(int shift = 16; shift >= 0; shift -= 8)
				{
					result ^= static_cast<uint8_t>(value >> shift);
					result *= 1099511628211ull;
				}
			}
		}
		return result;
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

	bool checkMachinedrumUpdaterLifecycle(const std::vector<uint8_t>& _rom,
		const std::string& _name)
	{
		synthLib::DeviceCreateParams params;
		params.romData = _rom;
		params.romName = _name;
		params.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
		md::Device device(params);
		if(!device.isValid() || !device.getHardware().isFirmwareUpdateDirectBoot())
		{
			std::fputs("user-supplied updater did not enter its initial direct boot\n", stderr);
			return false;
		}
		// DAWs may autosave immediately after constructing the plug-in. Reopening
		// that state must restart the updater, not boot the partially programmed
		// flash image that happened to exist at the autosave instant.
		std::vector<uint8_t> earlyState;
		md::Device reopened(params);
		if(!device.getState(earlyState, synthLib::StateTypeGlobal)
			|| !reopened.setState(earlyState, synthLib::StateTypeGlobal)
			|| !reopened.isValid()
			|| !reopened.getHardware().isFirmwareUpdateDirectBoot()
			|| !reopened.getHardware().isProjectStateRestorePending())
		{
			std::fputs("updater autosave did not reopen as a resumable initialization\n",
				stderr);
			return false;
		}

		const auto advanceUntil = [&](const auto& _ready,
			const std::chrono::seconds _timeout)
		{
			const auto deadline = std::chrono::steady_clock::now() + _timeout;
			while(!_ready(device.getHardware()))
			{
				device.getHardware().advance(256);
				if(device.getHardware().hasDspExecutionFault())
				{
					std::fprintf(stderr, "DSP%u halted at PC %06x during updater lifecycle\n",
						device.getHardware().dspExecutionFaultIndex() + 1,
						device.getHardware().dspExecutionFaultPc());
					return false;
				}
				if(std::chrono::steady_clock::now() >= deadline)
					return false;
			}
			return true;
		};

		if(!advanceUntil([](const md::Hardware& hardware)
			{
				return hardware.isFactoryFlashReadyForReboot();
			}, std::chrono::seconds(120)))
		{
			std::fputs("updater flash did not settle for its first restart\n", stderr);
			return false;
		}
		if(device.getHardware().isPlusDriveReadyForFactoryReboot())
		{
			std::fputs("updater incorrectly prepared +Drive before its first restart\n",
				stderr);
			return false;
		}

		const auto installedFlash = device.getHardware().copyFlashData();
		const auto firstCache = device.getHardware().copyFactoryFlashCache();
		// Installing the bootstrap must preserve the updater payload region that
		// follows the blocks it deliberately erased.
		if(installedFlash.size() < md::g_uwFlashSectorSize
			|| !std::equal(_rom.begin() + 0x4000,
				_rom.begin() + md::g_uwFlashSectorSize,
				installedFlash.begin() + 0x4000))
		{
			std::fputs("updater bootstrap erased its preserved flash payload\n", stderr);
			return false;
		}
		std::vector<uint8_t> firstState;
		const auto firstDrive = device.getHardware().copyPlusDriveData();
		if(firstCache.empty()
			|| !device.getHardware().replacePlusDriveData(firstDrive, true)
			|| !device.getHardware().plusDriveDirty()
			|| !device.getState(firstState, synthLib::StateTypeGlobal))
		{
			std::fputs("could not capture installed updater state\n", stderr);
			return false;
		}
		auto firstRestart = md::Device::prepareState(device.getPreparationContext(),
			firstState, synthLib::StateTypeGlobal, firstCache);
		if(!firstRestart || !device.commitPreparedState(*firstRestart)
			|| device.hardwareEpoch() != 2
			|| !device.getHardware().plusDriveDirty())
		{
			std::fputs("first updater restart transaction failed\n", stderr);
			return false;
		}
		auto& coldBoot = device.getHardware();
		if(coldBoot.isFirmwareUpdateDirectBoot()
			|| !coldBoot.isFactoryFlashInitializationExpected()
			|| coldBoot.copyFlashData() != installedFlash)
		{
			std::fputs("first restart did not cold-boot the installed flash\n", stderr);
			return false;
		}

		if(!advanceUntil([](const md::Hardware& hardware)
			{
				return hardware.isAudioReady()
					&& hardware.isFactoryFlashReadyForReboot()
					&& hardware.isPlusDriveReadyForFactoryReboot();
			}, std::chrono::seconds(120)))
		{
			std::fputs("post-updater DSP/+Drive preparation did not finish\n", stderr);
			return false;
		}

		const auto formattedDrive = device.getHardware().copyPlusDriveData();
		const auto finalCache = device.getHardware().copyFactoryFlashCache();
		std::vector<uint8_t> finalState;
		if(md::PlusDrive::isBlankStorage(formattedDrive) || finalCache.empty()
			|| !device.getState(finalState, synthLib::StateTypeGlobal))
		{
			std::fputs("post-updater storage was not ready for final restart\n", stderr);
			return false;
		}
		auto finalRestart = md::Device::prepareState(device.getPreparationContext(),
			finalState, synthLib::StateTypeGlobal, finalCache);
		if(!finalRestart || !device.commitPreparedState(*finalRestart)
			|| device.hardwareEpoch() != 3
			|| device.getHardware().isFirmwareUpdateDirectBoot()
			|| device.getHardware().isFactoryFlashInitializationExpected())
		{
			std::fputs("final post-updater restart transaction failed\n", stderr);
			return false;
		}
		if(!advanceUntil([](const md::Hardware& hardware)
			{
				return hardware.isAudioReady();
			}, std::chrono::seconds(120)))
		{
			std::fputs("final post-updater DSP boot did not become ready\n", stderr);
			return false;
		}

		auto fresh = std::make_unique<md::Hardware>(
			_rom, _name, md::MachineModel::Machinedrum,
			device.getHardware().copyPatchRam(),
			std::shared_ptr<md::FrontPanelPublisher>{},
			std::shared_ptr<md::MidiSysexTransferProgressPublisher>{},
			device.getHardware().copyFlashData(), finalCache,
			md::FlashSectorOverlay{}, formattedDrive);
		if(!fresh->isValid() || fresh->isFirmwareUpdateDirectBoot()
			|| fresh->isFactoryFlashInitializationExpected())
		{
			std::fputs("fresh initialized instance re-entered updater preparation\n",
				stderr);
			return false;
		}
		auto lateCheckpoint = std::make_unique<md::Hardware>(
			_rom, _name, md::MachineModel::Machinedrum,
			device.getHardware().copyPatchRam(),
			std::shared_ptr<md::FrontPanelPublisher>{},
			std::shared_ptr<md::MidiSysexTransferProgressPublisher>{},
			device.getHardware().copyFlashData(), finalCache);
		if(!lateCheckpoint->isFactoryFlashInitializationExpected()
			|| !lateCheckpoint->installStartupPlusDriveData(formattedDrive)
			|| lateCheckpoint->isFactoryFlashInitializationExpected())
		{
			std::fputs("late standalone checkpoint scheduled a redundant first-run reboot\n",
				stderr);
			return false;
		}
		return true;
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
		|| !checkFlashCommands()
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
	if(_argc < 2 || _argc > 3)
	{
		std::fprintf(stderr,
			"usage: mdfirmwareupdate_test [official-os-update.syx] [user-data.syx]\n");
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
	if(converted && model == md::MachineModel::Machinedrum)
	{
		if(!checkMachinedrumUpdaterLifecycle(rom, _argv[1]))
			return 1;
		std::printf("mdfirmwareupdate_test: PASS (Machinedrum two-stage updater lifecycle)\n");
		return 0;
	}
	auto hardware = std::make_unique<md::Hardware>(rom, _argv[1], model);
	if(!hardware->isValid())
	{
		std::fputs("direct boot initialization failed\n", stderr);
		return 1;
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	for(;;)
	{
		const auto panel = hardware->getFrontPanelSnapshot();
		if(panel.countLitPixels() > 2000
			|| (panel.getTileWriteCount() >= 64 && panel.countLitPixels() >= 100))
			break;
		hardware->advance(64);
		if(std::chrono::steady_clock::now() >= deadline)
		{
			std::fprintf(stderr, "boot did not reach the front panel: uc=%08x\n",
				hardware->getUC().getPC());
			return 1;
		}
	}
	if(model == md::MachineModel::Monomachine && (converted || _argc == 3))
	{
		auto transferBytes = _argc == 3 ? load(_argv[2]) : std::vector<uint8_t>{
			0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x54, 0xf7};
		if(md::validateMidiSysexStream(transferBytes, model)
			!= md::MidiSysexStreamValidation::Valid)
		{
			std::fputs("user-data SysEx failed stream validation\n", stderr);
			return 1;
		}
		const auto waveBankBefore = fingerprintDigiProBank(*hardware);
		auto prepared = md::prepareMidiSysexTransfer(std::move(transferBytes));
		if(!prepared || !hardware->startMidiSysexTransfer(*prepared))
		{
			std::fputs("converted Monomachine transfer did not start\n", stderr);
			return 1;
		}
		const auto transferDeadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(90);
		uint8_t payloadSpeed = 1;
		for(;;)
		{
			hardware->advance(64);
			const auto progress = hardware->getMidiSysexTransferProgress();
			if(progress.state == md::MidiSysexTransferState::Sending)
				payloadSpeed = progress.speedCode;
			if(progress.state == md::MidiSysexTransferState::Complete)
				break;
			if(std::chrono::steady_clock::now() >= transferDeadline)
			{
				std::fputs("converted Monomachine SysEx delivery timed out\n", stderr);
				return 1;
			}
		}
		auto waveBankAfter = fingerprintDigiProBank(*hardware);
		std::printf("Monomachine user-data transfer: %sx, DigiPRO=%016llx -> %016llx\n",
			md::midiTurboSpeedLabel(payloadSpeed),
			static_cast<unsigned long long>(waveBankBefore),
			static_cast<unsigned long long>(waveBankAfter));
		writeLcdPgm(*hardware);
		// This harness deliberately does not navigate a private firmware menu.
		// Completion proves hardware-boundary delivery only; installation belongs
		// to a separate panel-driven integration test.
	}
	std::printf("mdfirmwareupdate_test: PASS (%s, %zu-byte Gearmulator image)\n",
		model == md::MachineModel::Monomachine ? "Monomachine" : "Machinedrum",
		rom.size());
	return 0;
}
