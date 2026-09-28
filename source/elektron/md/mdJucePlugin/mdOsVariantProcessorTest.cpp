#include "mdPluginProcessor.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdosimage.h"
#include "mdLib/mdosvariant.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"
#include "synthLib/romLoader.h"

#include "juce_audio_utils/juce_audio_utils.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	std::string envPath(const char* const _name)
	{
		const auto* const value = std::getenv(_name);
		return value && *value ? value : std::string{};
	}

	std::string testHome()
	{
		const auto tmp = envPath("TMPDIR").empty() ? "/tmp" : envPath("TMPDIR");
		const auto dir = baseLib::filesystem::validatePath(tmp)
			+ "mdOsVariantProcessorTest/";
		baseLib::filesystem::createDirectory(dir);
		require(baseLib::filesystem::exists(dir), "temp dir");
		return dir;
	}

	std::string x13Path(const std::string& _sysexDir)
	{
		return _sysexDir
			+ "/custom-firmware-md-mm/Machinedrum_SPS1-UW_OS_X.13.syx";
	}

	md::OsUpgradeDecodeResult decodeX13(const std::string& _sysexDir)
	{
		std::vector<uint8_t> bytes;
		require(baseLib::filesystem::readFile(bytes, x13Path(_sysexDir)),
			"read X.13 file");
		md::OsUpgradeDecodeResult decoded;
		require(md::decodeOsUpgradeSysex(decoded, bytes), "decode X.13 file");
		return decoded;
	}

	mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig configFor(
		const std::string& _home)
	{
		mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig config;
		config.deviceHomePath = _home;
		return config;
	}

	std::vector<uint8_t> liveFlash(
		mdJucePlugin::AudioPluginAudioProcessor& _processor)
	{
		return _processor.getPlugin().withDeviceLocked(
			[](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				require(device != nullptr, "processor owns no local MD device");
				return device->getHardware().copyFlashData();
			});
	}
}

int main()
{
	const auto romPath = envPath("GEARMULATOR_MD_FIRMWARE_BIN");
	const auto sysexDir = envPath("GEARMULATOR_MDMM_SYSEX_DIR");
	if(romPath.empty() || sysexDir.empty())
	{
		std::cout << "mdOsVariantProcessorTest: SKIP (GEARMULATOR_MD_FIRMWARE_BIN and GEARMULATOR_MDMM_SYSEX_DIR required)\n";
		return 77;
	}

	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		synthLib::RomLoader::addSearchPath(
			juce::File(romPath).getParentDirectory().getFullPathName().toStdString());

		const auto home = testHome();
		const auto decoded = decodeX13(sysexDir);

		// Register the X.13 variant directly so the fingerprint is known;
		// the in-app import below must then accept the same file idempotently.
		std::vector<uint8_t> romBytes;
		require(baseLib::filesystem::readFile(romBytes, romPath), "read MD ROM");
		md::OsVariant variant;
		require(md::assembleOsVariant(variant, romBytes, decoded, "X.13"),
			"assemble X.13 variant");
		std::string variantFile, error;
		require(md::saveOsVariant(variantFile, home, variant, error),
			error.c_str());

		// Fresh instance, no selection anywhere: legacy boot, fingerprint zero.
		auto proc1 = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(
			md::MachineModel::Machinedrum, configFor(home), false);
		require(proc1->getOsVariantFingerprint() == 0, "fresh instance not default");

		// In-app import of the same file registers idempotently.
		juce::String result;
		require(proc1->importOsVariant(juce::File(x13Path(sysexDir)), result),
			result.toStdString().c_str());
		std::vector<md::OsVariantInfo> registered;
		require(md::listOsVariants(registered, home)
			&& registered.size() == 1
			&& registered.front().fingerprint == variant.fingerprint,
			"import did not register X.13");

		// Import refuses foreign models and non-upgrade files.
		require(!proc1->importOsVariant(juce::File(romPath), result),
			"import accepted the raw ROM");
		const auto mmSysex = sysexDir
			+ "/official-firmware-md-mm/Elektron_Monomachine_SFX6-60_OS1.32B.syx";
		if(baseLib::filesystem::exists(mmSysex))
		{
			require(!proc1->importOsVariant(juce::File(mmSysex), result),
				"import accepted a Monomachine file");
		}

		// Live switch reboots into the variant and records the instance
		// selection; the committed image carries the variant OS.
		require(proc1->switchOsVariant(variant.fingerprint, result),
			result.toStdString().c_str());
		require(proc1->getOsVariantFingerprint() == variant.fingerprint,
			"instance selection not recorded");
		// ...and becomes the default for fresh instances (standalone
		// relaunches), without touching saved projects.
		uint64_t defaultFp = 0;
		bool defaultFound = false;
		require(md::readOsVariantSelection(defaultFp, defaultFound, home)
			&& defaultFound && defaultFp == variant.fingerprint,
			"switch did not update the default OS");
		const auto switched = liveFlash(*proc1);
		require(switched.size() == md::g_romSize, "switched flash size");
		require(std::equal(switched.begin() + decoded.baseAddress,
			switched.begin() + decoded.endAddress,
			variant.image.begin() + decoded.baseAddress),
			"switched machine does not run the variant OS");

		// Settle the switched machine and seed its variant factory cache
		// file, mirroring what the running app's service does. Without
		// the file the relaunch below could not preinstall the baseline.
		{
			juce::AudioBuffer<float> audio(2, 128);
			juce::MidiBuffer midi;
			auto& audioProcessor = static_cast<juce::AudioProcessor&>(*proc1);
			audioProcessor.prepareToPlay(48000.0, 128);
			for(uint32_t i = 0; i < 20000; ++i)
			{
				audio.clear();
				midi.clear();
				audioProcessor.processBlock(audio, midi);
			}
			const auto seeded = proc1->getPlugin().withDeviceLocked(
				[](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					require(device != nullptr, "no local device to seed");
					std::string filename, error;
					md::FactoryFlashSnapshot snapshot;
					require(device->captureFactoryFlashCachePersistence(
						filename, snapshot, error), error.c_str());
					require(md::Device::materializeFactoryFlashCache(snapshot,
						device->getPreparationContext(), error), error.c_str());
					require(md::Device::writeFactoryFlashCachePersistence(
						filename, snapshot.cache, error), error.c_str());
					return true;
				});
			require(seeded, "variant cache seeding failed");
			audioProcessor.releaseResources();
		}

		// Quit/relaunch round trip through the host chunk: the relaunched
		// instance must restore the variant project (not fail against the
		// stock baseline) with the flash image intact, pre-boot.
		const auto preSaveFlash = liveFlash(*proc1);
		// The instance selection travels with the project chunk...
		juce::MemoryBlock hostState;
		static_cast<juce::AudioProcessor&>(*proc1).getStateInformation(hostState);
		auto proc2 = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(
			md::MachineModel::Machinedrum, configFor(home), false);
		static_cast<juce::AudioProcessor&>(*proc2).setStateInformation(
			hostState.getData(), static_cast<int>(hostState.getSize()));
		require(proc2->getOsVariantFingerprint() == variant.fingerprint,
			"project chunk did not carry the instance OS");
		const auto restoreResult = proc2->getPlugin().withDeviceLocked(
			[](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				require(device != nullptr, "relaunch owns no local device");
				return std::make_pair(device->projectStateRestoreStatus(),
					device->projectStateRestoreError());
			});
		require(restoreResult.first
				== md::Device::ProjectStateRestoreStatus::Idle,
			restoreResult.second.empty()
				? "relaunched project did not restore"
				: restoreResult.second.c_str());
		const auto postRestoreFlash = liveFlash(*proc2);
		require(postRestoreFlash == preSaveFlash,
			"relaunched project lost the variant flash image");
		std::cout << "variant project chunk round trip restored\n";

		// Fresh instances follow the shared default the switch just wrote
		// (this is what standalone relaunches boot); saved projects still
		// pin their own OS through the chunk above.
		auto proc3 = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(
			md::MachineModel::Machinedrum, configFor(home), false);
		require(proc3->getOsVariantFingerprint() == variant.fingerprint,
			"fresh instance did not follow the default OS");

		// A remembered global selection applies to fresh instances.
		require(md::writeOsVariantSelection(home, variant.fingerprint),
			"write global selection");
		auto proc4 = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(
			md::MachineModel::Machinedrum, configFor(home), false);
		require(proc4->getOsVariantFingerprint() == variant.fingerprint,
			"global selection not applied");
		const auto defaulted = liveFlash(*proc4);
		require(std::equal(defaulted.begin() + decoded.baseAddress,
			defaulted.begin() + decoded.endAddress,
			variant.image.begin() + decoded.baseAddress),
			"global selection did not boot the variant OS");
		require(md::clearOsVariantSelection(home), "clear global selection");
		// With no chunk and no default, fresh instances boot legacy stock.
		auto proc5 = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(
			md::MachineModel::Machinedrum, configFor(home), false);
		require(proc5->getOsVariantFingerprint() == 0,
			"cleared default did not fall back to stock");

		std::cout << "mdOsVariantProcessorTest passed\n";
		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "mdOsVariantProcessorTest: " << e.what() << '\n';
		return 1;
	}
}
