#include "mdAutomationTestSupport.h"

#include "mdLib/mdplusdrive.h"

#include "baseLib/filesystem.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace
{
	using namespace mdAutomationTest;

	pluginLib::Parameter& parameter(mdJucePlugin::Controller& _controller)
	{
		auto* const result = _controller.getParameter("Level", 0);
		require(result != nullptr, "test parameter was not registered");
		return *result;
	}

	std::vector<uint8_t> makePlusDriveImage(const uint32_t _sector,
		const uint8_t _value)
	{
		md::PlusDrive drive;
		auto image = drive.copyStorage();
		image[15] = 1;
		image.push_back(static_cast<uint8_t>(_sector >> 24));
		image.push_back(static_cast<uint8_t>(_sector >> 16));
		image.push_back(static_cast<uint8_t>(_sector >> 8));
		image.push_back(static_cast<uint8_t>(_sector));
		image.insert(image.end(), 512, _value);
		require(md::PlusDrive::validateStorage(image),
			"test +Drive image was invalid");
		return image;
	}

	std::vector<uint8_t> copyPlusDrive(Harness& _harness)
	{
		std::vector<uint8_t> result;
		require(_harness.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device)
					return false;
				result = device->getHardware().copyPlusDriveData();
				return true;
			}), "could not capture +Drive data");
		return result;
	}

	std::vector<uint8_t> copyPlusDrive(
		mdJucePlugin::AudioPluginAudioProcessor& _processor)
	{
		std::vector<uint8_t> result;
		require(_processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device)
					return false;
				result = device->getHardware().copyPlusDriveData();
				return true;
			}), "could not capture standalone +Drive data");
		return result;
	}

	void installPlusDrive(Harness& _harness, const std::vector<uint8_t>& _image)
	{
		require(_harness.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				return device && device->getHardware().replacePlusDriveData(_image, true);
			}), "could not install test +Drive data");
	}

	void installPlusDrive(mdJucePlugin::AudioPluginAudioProcessor& _processor,
		const std::vector<uint8_t>& _image)
	{
		require(_processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				return device && device->getHardware().replacePlusDriveData(_image, true);
			}), "could not install standalone test +Drive data");
	}

	bool waitForImage(const juce::File& _file,
		const std::vector<uint8_t>& _expected)
	{
		for(int attempt = 0; attempt < 100; ++attempt)
		{
			std::vector<uint8_t> actual;
			if(baseLib::filesystem::readFile(actual,
				_file.getFullPathName().toStdString()) && actual == _expected)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}

	void verifyStandaloneFilterStateMigration(Harness& _harness,
		const std::vector<uint8_t>& _imageA,
		const std::vector<uint8_t>& _imageB)
	{
		installPlusDrive(_harness, _imageA);
		juce::MemoryBlock legacyFilterState;
		_harness.audioProcessor.getStateInformation(legacyFilterState);
		require(!legacyFilterState.isEmpty(),
			"could not capture legacy JUCE standalone filterState");

		const auto directory = juce::File::getCurrentWorkingDirectory()
			.getNonexistentChildFile("gearmulator-md-filterstate-migration", {}, false);
		require(directory.createDirectory().wasOk(),
			"could not create isolated standalone migration directory");
		struct Cleanup final
		{
			juce::File directory;
			~Cleanup() { directory.deleteRecursively(); }
		} cleanup{directory};
		const auto checkpoint = directory.getChildFile("standalone.mdpd");

		{
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig config;
			config.standalonePlusDriveFile = checkpoint;
			mdJucePlugin::AudioPluginAudioProcessor standalone(
				md::MachineModel::Machinedrum, std::move(config), false);
			static_cast<juce::AudioProcessor&>(standalone).setStateInformation(
				legacyFilterState.getData(),
				static_cast<int>(legacyFilterState.getSize()));
			require(waitForImage(checkpoint, _imageA),
				"first standalone launch did not migrate JUCE filterState into its checkpoint");

			installPlusDrive(standalone, _imageB);
		}
		require(waitForImage(checkpoint, _imageB),
			"clean standalone quit did not flush the unsettled +Drive change");

		{
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig config;
			config.standalonePlusDriveFile = checkpoint;
			mdJucePlugin::AudioPluginAudioProcessor relaunched(
				md::MachineModel::Machinedrum, std::move(config), false);
			require(copyPlusDrive(relaunched) == _imageB,
				"standalone relaunch did not restore the dedicated checkpoint");

			// JUCE's StandalonePluginHolder reloads filterState after constructing the
			// processor. Once the dedicated checkpoint exists, that older blob must not
			// roll the +Drive back during every launch.
			static_cast<juce::AudioProcessor&>(relaunched).setStateInformation(
				legacyFilterState.getData(),
				static_cast<int>(legacyFilterState.getSize()));
			require(copyPlusDrive(relaunched) == _imageB,
				"stale JUCE filterState overrode the standalone checkpoint");
			require(waitForImage(checkpoint, _imageB),
				"stale JUCE filterState changed the standalone checkpoint");
		}
	}

	bool firmwareAudioReady(Harness& _harness)
	{
		bool ready = false;
		_harness.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
					ready = device->getHardware().isAudioReady();
			});
		return ready;
	}

	struct MdBootStatus
	{
		bool audioReady = false;
		bool factoryFlashReady = false;
		size_t plusDriveSectors = 0;
		uint64_t plusDriveCommands = 0;
	};

	MdBootStatus mdBootStatus(Harness& _harness)
	{
		MdBootStatus status;
		_harness.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				if(auto* const device = dynamic_cast<md::Device*>(_device))
				{
					auto& hardware = device->getHardware();
					status.audioReady = hardware.isAudioReady();
					status.factoryFlashReady = hardware.factoryFlashCacheReady();
					status.plusDriveSectors = hardware.getUC().getSim()
						.getPlusDrive().storedSectorCount();
					status.plusDriveCommands = hardware.getUC().getSim()
						.getPlusDrive().commandCount();
				}
			});
		return status;
	}

	void verifyPlusDriveLifecycle(Harness& _harness)
	{
		const auto imageA = makePlusDriveImage(7, 0x5a);
		const auto imageB = makePlusDriveImage(9, 0x27);
		installPlusDrive(_harness, imageA);

		// A second running instance must own an independent image even when both
		// devices started from the same one-time legacy migration source.
		Harness second(md::MachineModel::Machinedrum);
		if(second.hasLocalFirmware())
		{
			const auto secondBefore = copyPlusDrive(second);
			installPlusDrive(_harness, imageB);
			require(copyPlusDrive(second) == secondBefore,
				"one MD instance changed another instance's +Drive");
			installPlusDrive(_harness, imageA);
		}

		const auto nonce = std::chrono::steady_clock::now()
			.time_since_epoch().count();
		const auto directory = juce::File::getCurrentWorkingDirectory();
		const auto exported = directory.getChildFile(
			"gearmulator-md-plusdrive-export-" + juce::String(nonce) + ".mdpd");
		struct Cleanup final
		{
			const juce::File& file;
			~Cleanup()
			{
				file.deleteFile();
			}
		} cleanup{exported};
		exported.deleteFile();

		juce::String result;
		require(_harness.processor.exportPlusDriveImage(exported, result),
			"explicit +Drive export failed: " + result.toStdString());
		installPlusDrive(_harness, imageB);
		require(_harness.processor.importPlusDriveImage(exported, result),
			"explicit +Drive import failed: " + result.toStdString());
		require(copyPlusDrive(_harness) == imageA,
			"import did not restore the exported +Drive image");

		const std::vector<uint8_t> corrupt{1, 2, 3, 4};
		require(baseLib::filesystem::writeFileAtomic(
			exported.getFullPathName().toStdString(), corrupt),
			"could not create corrupt +Drive test input");
		require(!_harness.processor.importPlusDriveImage(exported, result),
			"corrupt +Drive import was accepted");
		require(copyPlusDrive(_harness) == imageA,
			"failed +Drive import changed live storage");

		// Reboot must retain all battery-backed Snapshot bytes (kits, patterns,
		// songs and globals), UW flash, and the project-owned +Drive.
		std::vector<uint8_t> stateBefore;
		require(_harness.processor.getPlugin().getState(
			stateBefore, synthLib::StateTypeGlobal),
			"could not capture state before reboot");
		require(_harness.processor.rebootDevice(),
			"transactional Machinedrum reboot failed");
		std::vector<uint8_t> stateAfter;
		require(_harness.processor.getPlugin().getState(
			stateAfter, synthLib::StateTypeGlobal),
			"could not capture state after reboot");
		require(stateAfter == stateBefore,
			"reboot changed project-owned Machinedrum storage");

		verifyStandaloneFilterStateMigration(_harness, imageA, imageB);
	}

	void verifyModel(const md::MachineModel _model)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
		{
			std::cout << "mdAutomationFirmwareTest: SKIP "
				<< modelName(_model)
				<< " (firmware unavailable)\n";
			return;
		}
		harness.prepare();

		auto& audioProcessor = harness.audioProcessor;
		auto& controller = harness.controller;
		if(_model == md::MachineModel::Machinedrum)
		{
			// A blank +Drive is formatted during the first 1.63 boot. The original
			// automation request predates that work, and the firmware asks for a
			// reboot afterward. Preserve the newly formatted project drive across that
			// reboot, then ask for automation state again.
			MdBootStatus firstBoot;
			MdBootStatus previousBoot;
			int stableIntervals = 0;
			for(int attempt = 0; attempt < 30; ++attempt)
			{
				harness.process(1000);
				firstBoot = mdBootStatus(harness);
				if(firstBoot.plusDriveSectors != 0
					&& firstBoot.plusDriveSectors == previousBoot.plusDriveSectors
					&& firstBoot.plusDriveCommands == previousBoot.plusDriveCommands)
					++stableIntervals;
				else
					stableIntervals = 0;
				previousBoot = firstBoot;
				if(firstBoot.audioReady && firstBoot.factoryFlashReady
					&& stableIntervals >= 2)
					break;
			}
			std::cerr << "MD first boot: audio " << firstBoot.audioReady
				<< ", factory flash " << firstBoot.factoryFlashReady
				<< ", +Drive sectors " << firstBoot.plusDriveSectors
				<< ", commands " << firstBoot.plusDriveCommands << '\n';
			require(firstBoot.audioReady && firstBoot.factoryFlashReady
				&& stableIntervals >= 2,
				"Machinedrum did not finish formatting its blank +Drive");
			require(harness.processor.rebootDevice(),
				"post-format Machinedrum reboot failed");
			require(mdBootStatus(harness).factoryFlashReady,
				"Machinedrum reboot discarded its validated in-memory UW factory cache");
			// Audio-ready goes true before the main CPU has finished the +Drive boot
			// path. Match the 20-second firmware acceptance test before sending SysEx.
			harness.process(9000);
				require(firmwareAudioReady(harness),
					"Machinedrum audio did not become ready after +Drive format");
			controller.requestAutomationState();
		}
		require(harness.synchronize(),
			"initial firmware synchronization timed out (global "
			+ std::to_string(controller.hasAutomationGlobalSnapshot()) + ", kit "
			+ std::to_string(controller.hasAutomationKitSnapshot()) + ", base "
			+ std::to_string(controller.getAutomationBaseChannel()) + ", requests="
			+ std::to_string(controller.getSynchronizationRequestCount()) + ", "
			+ harness.firmwareReadiness() + ")");
		require(controller.getAutomationBaseChannel() < 16,
			"firmware Global has MIDI base channel set to NONE");

		auto& probe = parameter(controller);
		const auto beforeTransmitCount =
			controller.getTransmittedAutomationChangeCount();

		// Make the cache say zero without touching firmware, then require an explicit
		// repeated/default host write to reach and be consumed by the firmware UART.
		probe.setValueFromSynth(0, pluginLib::Parameter::Origin::PresetChange);
		const auto beforeZero = harness.telemetry();
		hostWrite(probe, 0);
		harness.process(32);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeTransmitCount + 1,
			"host write did not produce exactly one automation CC");
		const auto afterZero = harness.telemetry();
		require(afterZero.consumed >= beforeZero.consumed + 3
			&& afterZero.overflows == beforeZero.overflows,
			"explicit zero automation CC was not consumed losslessly by firmware");

		const auto beforeChanged = harness.telemetry();
		hostWrite(probe, 127);
		harness.process(32);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeTransmitCount + 2,
			"changed host write did not produce exactly one automation CC");
		const auto afterChanged = harness.telemetry();
		require(afterChanged.consumed >= beforeChanged.consumed + 3
			&& afterChanged.overflows == beforeChanged.overflows,
			"changed automation CC was not consumed losslessly by firmware");

		const auto beforeStressCount =
			controller.getTransmittedAutomationChangeCount();
		const auto beforeStressMidi = harness.telemetry();
		size_t expectedWrites = 0;
		int ordinal = 0;
		for(auto* const audioParameter : audioProcessor.getParameters())
		{
			auto* const automationParameter = dynamic_cast<pluginLib::Parameter*>(
				audioParameter);
			if(!automationParameter)
				continue;
			const auto& description = automationParameter->getDescription();
			const auto mutePage = _model == md::MachineModel::Monomachine
				? md::automation::monomachine::Mute
				: md::automation::machinedrum::Mute;
			if(description.page == mutePage)
				continue;
			const auto value = (ordinal * 37 + 19) & 0x7f;
			hostWrite(*automationParameter, value);
			++expectedWrites;
			if((++ordinal & 31) == 0)
				harness.process(4);
		}
		harness.process(64);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeStressCount + expectedWrites,
			"automation stress pass did not transmit every host write");
		const auto afterStressMidi = harness.telemetry();
		require(afterStressMidi.consumed
			>= beforeStressMidi.consumed + expectedWrites * 3
			&& afterStressMidi.overflows == beforeStressMidi.overflows,
			"automation stress stream was not consumed losslessly by firmware");

		hostWrite(probe, 47);
		harness.process(32);
		juce::MemoryBlock savedState;
		audioProcessor.getStateInformation(savedState);
		require(!savedState.isEmpty(), "processor produced empty persisted state");
		hostWrite(probe, 12);
		harness.process(32);
		require(probe.getUnnormalizedValue() == 12,
			"pre-restore control write failed");
		audioProcessor.setStateInformation(savedState.getData(),
			static_cast<int>(savedState.getSize()));
		const auto beforeRestoreFlush =
			controller.getTransmittedAutomationChangeCount();
		require(harness.synchronize(),
			"state restore did not resynchronize automation");
		require(probe.getUnnormalizedValue() == 47,
			"firmware-backed automation value was not restored from DAW state");
		// Synchronization can complete from inside the audio callback while its
		// realtime drain guard is held. The message-thread completion path must not
		// wait on that guard; the next bounded callback performs the replay instead.
		harness.process(32);
		const auto restoredWrites = controller.getTransmittedAutomationChangeCount()
			- beforeRestoreFlush;
		require(restoredWrites >= audioProcessor.getParameters().size(),
			"DAW-state automation snapshot flushed "
			+ std::to_string(restoredWrites) + " of "
			+ std::to_string(audioProcessor.getParameters().size())
			+ " values back to firmware");
		if(_model == md::MachineModel::Machinedrum)
			verifyPlusDriveLifecycle(harness);

		std::cout << "mdAutomationFirmwareTest: "
			<< modelName(_model)
			<< " PASS\n";
	}
}

int main(const int _argc, const char* const* _argv)
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		const auto only = _argc > 1 ? std::string(_argv[1]) : std::string();
		if(only != "--mm")
			verifyModel(md::MachineModel::Machinedrum);
		if(only != "--md")
			verifyModel(md::MachineModel::Monomachine);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdAutomationFirmwareTest: " << error.what() << '\n';
		return 1;
	}
}
