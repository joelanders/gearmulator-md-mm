#include "mdPluginProcessor.h"

#include "mdController.h"
#include "mdPluginEditorState.h"
#include "mdStandalonePlusDrivePersistence.h"
#include "mdStorageImage.h"

// ReSharper disable once CppUnusedIncludeDirective
#include "BinaryData.h"
#include "jucePluginLib/processorPropertiesInit.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdplusdrive.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"

#include "synthLib/deviceException.h"

#include "baseLib/binarystream.h"

#include <cstdio>
#include <utility>

namespace
{
	#if defined(MD_JUCEPLUGIN_MONOMACHINE)
	constexpr auto g_defaultModel = md::MachineModel::Monomachine;
	#else
	constexpr auto g_defaultModel = md::MachineModel::Machinedrum;
	#endif

	const char* productName(const md::MachineModel _model)
	{
		return _model == md::MachineModel::Monomachine ? "Gearmulator MM" : "Gearmulator MD";
	}

	const char* dataFolderName(const md::MachineModel _model)
	{
		return _model == md::MachineModel::Monomachine ? "Monomachine" : "Machinedrum";
	}

	juce::PropertiesFile::Options getOptions(const md::MachineModel _model,
		const bool _ephemeral)
	{
		juce::PropertiesFile::Options opts;
		const auto suffix = _model == md::MachineModel::Monomachine
			? "Monomachine" : "Machinedrum";
		opts.applicationName = _ephemeral
			? juce::String("DSP56300EmulatorMachineRackEditorIdentityTest_") + suffix
				+ "_" + juce::Uuid().toString()
			: juce::String("DSP56300Emulator") + suffix;
		opts.filenameSuffix = ".settings";
		opts.folderName = opts.applicationName;
		opts.osxLibrarySubFolder = "Application Support/" + opts.applicationName;
		opts.doNotSave = _ephemeral;
		if(_ephemeral)
			opts.millisecondsBeforeSaving = -1;
		return opts;
	}

	pluginLib::Processor::Properties makeProcessorProperties(const md::MachineModel _model)
	{
		const auto compiled = pluginLib::initProcessorProperties();
		return {
			productName(_model), compiled.vendor, compiled.isSynth,
			compiled.wantsMidiInput, compiled.producesMidiOut, compiled.isMidiEffect,
			_model == md::MachineModel::Monomachine ? "Tmno" : "Tmdr",
			compiled.lv2Uri, compiled.binaryData, dataFolderName(_model)
		};
	}
}

namespace mdJucePlugin
{
	void AudioPluginAudioProcessor::saveChunkData(baseLib::BinaryStream& _stream)
	{
		jucePluginEditorLib::Processor::saveChunkData(_stream);
		const auto& controller = dynamic_cast<const Controller&>(getController());
		const auto snapshot = controller.createAutomationSnapshot();
		if(!snapshot.empty())
		{
			baseLib::ChunkWriter chunk(_stream, "AUTO", 1);
			_stream.write(snapshot);
		}
	}

	void AudioPluginAudioProcessor::loadChunkData(baseLib::ChunkReader& _reader)
	{
		jucePluginEditorLib::Processor::loadChunkData(_reader);
		_reader.add("AUTO", 1, [this](baseLib::BinaryStream& _stream, uint32_t)
		{
			std::vector<uint8_t> snapshot;
			_stream.read(snapshot);
			auto& controller = dynamic_cast<Controller&>(getController());
			(void)controller.restoreAutomationSnapshot(snapshot);
		});
	}

	bool AudioPluginAudioProcessor::loadCustomData(
		const std::vector<uint8_t>& _sourceBuffer)
	{
		std::unique_lock<std::mutex> operationLock;
		uint64_t preservedEpoch = 0;
		uint64_t preservedGeneration = 0;
		bool preservedDirty = false;
		std::vector<uint8_t> preservedDrive;
		bool preserveStandaloneDrive = false;
		bool wasAuthoritative = false;
		if(m_standalonePlusDrive)
		{
			operationLock = std::unique_lock<std::mutex>(m_storageLoadMutex);
			wasAuthoritative = m_standalonePlusDrive->authoritative();
			if(capturePlusDrivePersistenceSnapshot(true, preservedEpoch,
				preservedGeneration, preservedDirty, preservedDrive))
				preserveStandaloneDrive = wasAuthoritative;
		}

		const bool loaded = jucePluginEditorLib::Processor::loadCustomData(_sourceBuffer);
		if(!loaded || !m_standalonePlusDrive)
			return loaded;

		if(preserveStandaloneDrive)
		{
			const bool restored = getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					return device && device->getModel() == md::MachineModel::Machinedrum
						&& device->getHardware().replacePlusDriveData(
							preservedDrive, preservedDirty);
				});
			if(!restored)
			{
				m_standalonePlusDrive->blockWrites(
					"Standalone state loaded, but its authoritative +Drive could not be restored");
				return false;
			}
			if(preservedDirty || !wasAuthoritative)
				m_standalonePlusDrive->requestFlush(true);
		}
		else
		{
			// First launch migration: when no dedicated checkpoint exists, adopt the
			// +Drive from JUCE's legacy standalone filterState exactly once.
			m_standalonePlusDrive->requestFlush(true);
		}
		return loaded;
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor()
		: AudioPluginAudioProcessor(g_defaultModel)
	{
	}

	md::MachineModel AudioPluginAudioProcessor::getCompiledProductModel()
	{
		return g_defaultModel;
	}

	bool AudioPluginAudioProcessor::hasEmbeddedProductResource(const std::string_view _filename)
	{
		return pluginLib::Processor::findResource(
			makeProcessorProperties(g_defaultModel).binaryData, std::string(_filename)).has_value();
	}

	juce::File AudioPluginAudioProcessor::getInstalledFactoryStorageImage() const
	{
		return juce::File(juce::String::fromUTF8(getDataFolder().c_str()))
			.getChildFile("nvram").getChildFile("mm-factory-live3-be.bin");
	}

	juce::File AudioPluginAudioProcessor::getStorageRecoveryImage() const
	{
		return juce::File(juce::String::fromUTF8(getDataFolder().c_str()))
			.getChildFile("nvram").getChildFile("mm-storage-recovery.bin");
	}

	bool AudioPluginAudioProcessor::loadStorageImage(const juce::File& _source,
		juce::String& _result)
	{
		_result.clear();
		if(m_model != md::MachineModel::Monomachine)
		{
			_result = "Storage images are only supported by Gearmulator MM";
			return false;
		}

		std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
		if(!operationLock.owns_lock())
		{
			_result = "Another storage image is already being loaded";
			return false;
		}

		const auto recovery = getStorageRecoveryImage();
		const auto sourceTarget = _source.isSymbolicLink()
			? _source.getLinkedTarget() : _source;
		const auto recoveryTarget = recovery.isSymbolicLink()
			? recovery.getLinkedTarget() : recovery;
		// Reading the complete source before touching recovery intentionally permits
		// selecting the recovery image itself. That operation becomes an A/B swap:
		// the saved bytes are activated and the previously live bytes become recovery.
		// A symlink to recovery has the same deliberate swap semantics. A hard link
		// remains immutable because atomic promotion replaces only recovery's name.
		const bool restoringRecovery = _source == recovery || sourceTarget == recoveryTarget;

		std::vector<uint8_t> replacementBytes;
		juce::String ioError;
		if(!storageImage::readExact(_source, replacementBytes, ioError))
		{
			_result = "Storage was not changed. " + ioError;
			return false;
		}

		std::vector<uint8_t> replacementState;
		if(!md::encodeState(replacementState, replacementBytes,
			md::MachineModel::Monomachine, synthLib::StateTypeGlobal))
		{
			_result = "Storage was not changed. The image could not be prepared";
			return false;
		}

		std::shared_ptr<const md::Device::PreparationContext> preparationContext;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device);
				device && device->getModel() == md::MachineModel::Monomachine)
				preparationContext = device->getPreparationContext();
		});
		if(!preparationContext)
		{
			_result = "Storage was not changed. The machine is not using a local device";
			return false;
		}

		// Hardware construction is intentionally outside the Plugin lock. The live
		// machine continues processing while the replacement validates and boots.
		auto prepared = md::Device::prepareState(preparationContext, replacementState,
			synthLib::StateTypeGlobal);
		if(!prepared)
		{
			_result = "Storage was not changed. The machine rejected the image";
			return false;
		}

		// Capture the recovery point as late as possible, but perform all file I/O
		// after releasing the realtime Plugin lock.
		std::vector<uint8_t> previousBytes;
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getPreparationContext() != preparationContext)
					return false;
				previousBytes = device->getHardware().copyPatchRam();
				return previousBytes.size() == md::g_patchRamStateSize;
			});
		if(!captured)
		{
			_result = "Storage was not changed. The machine changed while the image was preparing";
			return false;
		}

		// Always replace the recovery directory entry itself. Following a recovery
		// symlink for writes could modify an otherwise read-only source image outside
		// the product data folder, contrary to the storage selector's safety promise.
		juce::String commitError;
		const bool committed = storageImage::installRecoveryThenCommit(
			recovery, previousBytes,
			[&]
			{
				return getPlugin().withDeviceLocked(
					[&](synthLib::Device* const _device)
					{
						auto* const device = dynamic_cast<md::Device*>(_device);
						if(!device
							|| device->getPreparationContext() != preparationContext)
						{
							commitError = "The machine changed before the final reboot.";
							return false;
						}

						// File I/O deliberately happens outside this lock. Refuse the
						// exchange if firmware or panel input changed battery-backed
						// storage in that interval; otherwise those newer bytes would
						// exist in neither the replacement nor its recovery image.
						if(device->getHardware().copyPatchRam() != previousBytes)
						{
							commitError = "Live storage was edited while the replacement was preparing; try again.";
							return false;
						}
						return device->commitPreparedState(*prepared);
					});
			}, ioError);
		if(!committed)
		{
			_result = "Storage was not changed. ";
			if(commitError.isNotEmpty())
				_result += commitError + " ";
			_result += ioError;
			return false;
		}

		// Successful commit leaves the retired Hardware in prepared. Destroy it only
		// after releasing the Plugin lock, then refresh every controller view.
		prepared.reset();
		if(hasController())
			getController().onStateLoaded();

		_result = restoringRecovery
			? "Previous storage restored; the machine rebooted. The storage that was active is now the recovery image at "
				+ recovery.getFullPathName()
			: "Storage image loaded; the machine rebooted. Previous storage was saved to "
				+ recovery.getFullPathName();
		return true;
	}

	bool AudioPluginAudioProcessor::replacePlusDrive(
		std::vector<uint8_t> _replacement, const juce::String& _operation,
		juce::String& _result)
	{
		_result.clear();
		if(m_model != md::MachineModel::Machinedrum)
		{
			_result = "+Drive images are only supported by Gearmulator MD";
			return false;
		}
		if(_replacement.size() > md::g_plusDriveMaxSerializedBytes
			|| !md::PlusDrive::validateStorage(_replacement))
		{
			_result = "The replacement is not a valid bounded sparse +Drive image";
			return false;
		}
		if(m_standalonePlusDrive && !m_standalonePlusDrive->ownsWriter())
		{
			_result = "Another standalone instance owns the persistent +Drive. This instance cannot replace it";
			return false;
		}
		std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
		if(!operationLock.owns_lock())
		{
			_result = "Another machine-storage operation is already running";
			return false;
		}

		md::Device* liveDevice = nullptr;
		uint64_t liveEpoch = 0;
		std::vector<uint8_t> originalState;
		std::vector<uint8_t> factoryFlashCache;
		std::shared_ptr<const md::Device::PreparationContext> preparationContext;
		bool initializationPending = false;
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				const auto& hardware = device->getHardware();
				if(hardware.isFactoryFlashInitializationExpected()
					&& (!hardware.isFactoryFlashReadyForReboot()
						|| !hardware.isPlusDriveReadyForFactoryReboot()))
				{
					initializationPending = true;
					return false;
				}
				liveDevice = device;
				liveEpoch = device->hardwareEpoch();
				preparationContext = device->getPreparationContext();
				factoryFlashCache = device->getHardware().copyFactoryFlashCache();
				return device->getState(originalState, synthLib::StateTypeGlobal);
			});
		if(!captured || !preparationContext)
		{
			_result = initializationPending
				? "Wait for first-run preparation and automatic reboot before replacing the +Drive"
				: "The local Machinedrum state could not be captured";
			return false;
		}

		auto replacementState = originalState;
		if(!md::replacePlusDriveState(replacementState, _replacement))
		{
			_result = "The current project state could not accept the +Drive image";
			return false;
		}
		auto prepared = md::Device::prepareState(preparationContext, replacementState,
			synthLib::StateTypeGlobal, factoryFlashCache);
		if(!prepared)
		{
			_result = "The replacement machine could not be prepared";
			return false;
		}

		juce::String commitError;
		const bool committed = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(device != liveDevice || !device || device->hardwareEpoch() != liveEpoch)
				{
					commitError = "The machine changed while the replacement was preparing";
					return false;
				}
				std::vector<uint8_t> currentState;
				if(!device->getState(currentState, synthLib::StateTypeGlobal)
					|| currentState != originalState)
				{
					commitError = "Machine storage was edited while the replacement was preparing; try again";
					return false;
				}
				return device->commitPreparedState(*prepared);
			});
		if(!committed)
		{
			_result = _operation + " was not applied. " + commitError;
			return false;
		}
		prepared.reset();
		if(hasController())
			getController().onStateLoaded();
		if(m_standalonePlusDrive)
			m_standalonePlusDrive->allowReplacementAndFlush();
		_result = _operation + " applied; the Machinedrum rebooted";
		return true;
	}

	bool AudioPluginAudioProcessor::importPlusDriveImage(const juce::File& _source,
		juce::String& _result)
	{
		std::vector<uint8_t> replacement;
		if(!storageImage::readPlusDrive(_source, replacement, _result))
			return false;
		return replacePlusDrive(std::move(replacement),
			"+Drive image '" + _source.getFileName() + "'", _result);
	}

	bool AudioPluginAudioProcessor::exportPlusDriveImage(const juce::File& _target,
		juce::String& _result)
	{
		std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
		if(!operationLock.owns_lock())
		{
			_result = "Another machine-storage operation is already running";
			return false;
		}
		return exportPlusDriveImageUnlocked(_target, _result);
	}

	bool AudioPluginAudioProcessor::exportPlusDriveImageUnlocked(
		const juce::File& _target, juce::String& _result)
	{
		_result.clear();
		if(m_model != md::MachineModel::Machinedrum)
		{
			_result = "+Drive images are only supported by Gearmulator MD";
			return false;
		}
		std::vector<uint8_t> data;
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				data = device->getHardware().copyPlusDriveData();
				return true;
			});
		if(!captured)
		{
			_result = "The local +Drive could not be captured";
			return false;
		}
		if(!storageImage::writePlusDriveAtomically(_target, data, _result))
			return false;
		_result = "+Drive image exported atomically to " + _target.getFullPathName();
		return true;
	}

	bool AudioPluginAudioProcessor::resetPlusDrive(juce::String& _result)
	{
		md::PlusDrive blank;
		return replacePlusDrive(blank.copyStorage(), "Blank +Drive", _result);
	}

	bool AudioPluginAudioProcessor::rebootMachinedrum(juce::String& _result)
	{
		_result.clear();
		if(m_model != md::MachineModel::Machinedrum)
		{
			_result = "Reboot is only available for Gearmulator MD";
			return false;
		}
		std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
		if(!operationLock.owns_lock())
		{
			_result = "Another machine-storage operation is already running";
			return false;
		}

		md::Device* liveDevice = nullptr;
		uint64_t liveEpoch = 0;
		std::vector<uint8_t> originalState;
		std::vector<uint8_t> factoryFlashCache;
		std::shared_ptr<const md::Device::PreparationContext> preparationContext;
		bool initializationPending = false;
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				const auto& hardware = device->getHardware();
				const bool firmwareUpdateRestartReady =
					hardware.isFirmwareUpdateDirectBoot()
					&& hardware.isFactoryFlashReadyForReboot();
				if(hardware.isFactoryFlashInitializationExpected()
					&& !firmwareUpdateRestartReady
					&& (!hardware.isFactoryFlashReadyForReboot()
						|| !hardware.isPlusDriveReadyForFactoryReboot()))
				{
					initializationPending = true;
					return false;
				}
				liveDevice = device;
				liveEpoch = device->hardwareEpoch();
				preparationContext = device->getPreparationContext();
				factoryFlashCache = device->getHardware().copyFactoryFlashCache();
				return device->getState(originalState, synthLib::StateTypeGlobal);
			});
		if(!captured || !preparationContext)
		{
			_result = initializationPending
				? "Factory samples or +Drive are still being prepared. Keep host audio processing active and wait for the automatic reboot"
				: "The project-owned Machinedrum state could not be captured";
			return false;
		}
		auto prepared = md::Device::prepareState(preparationContext, originalState,
			synthLib::StateTypeGlobal, factoryFlashCache);
		if(!prepared)
		{
			_result = "The replacement Machinedrum could not be prepared";
			return false;
		}

		juce::String commitError;
		const bool committed = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(device != liveDevice || !device || device->hardwareEpoch() != liveEpoch)
				{
					commitError = "The machine changed while the reboot was preparing";
					return false;
				}
				std::vector<uint8_t> currentState;
				if(!device->getState(currentState, synthLib::StateTypeGlobal)
					|| currentState != originalState)
				{
					commitError = "Machine storage was edited while the reboot was preparing; try again";
					return false;
				}
				return device->commitPreparedState(*prepared);
			});
		if(!committed)
		{
			_result = "The Machinedrum was not rebooted. " + commitError;
			return false;
		}
		prepared.reset();
		if(hasController())
			getController().onStateLoaded();
		_result = "The Machinedrum rebooted with its active +Drive";
		return true;
	}

	bool AudioPluginAudioProcessor::rebootDevice()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return jucePluginEditorLib::Processor::rebootDevice();
		juce::String result;
		return rebootMachinedrum(result);
	}

	bool AudioPluginAudioProcessor::serviceFactoryInitialization()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return false;

		enum class State { Waiting, FirmwareUpdateReady, Ready, NotNeeded };
		auto state = State::NotNeeded;
		bool factoryCacheReady = false;
		bool factoryCacheDisqualified = false;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			if(!device || device->getModel() != md::MachineModel::Machinedrum
				|| !device->isValid())
				return;
			const auto& hardware = device->getHardware();
			if(!hardware.isFactoryFlashInitializationExpected())
				return;
			factoryCacheReady = hardware.isFactoryFlashCacheReady();
			factoryCacheDisqualified =
				hardware.isFactoryFlashCaptureDisqualified();
			if(hardware.isFirmwareUpdateDirectBoot())
			{
				state = hardware.isFactoryFlashReadyForReboot()
					? State::FirmwareUpdateReady : State::Waiting;
				return;
			}
			state = hardware.isFactoryFlashReadyForReboot()
				&& hardware.isPlusDriveReadyForFactoryReboot()
				? State::Ready : State::Waiting;
		});

		if(state == State::NotNeeded)
		{
			// Host state restore can replace the Hardware after construction. Keep a
			// cheap monitor armed so a later cold image still receives its one-time
			// in-process reboot.
			startTimer(1000);
			return false;
		}
		if(state != State::Ready && state != State::FirmwareUpdateReady)
		{
			startTimer(250);
			return false;
		}

		std::string cacheError;
		bool cachePersisted = false;
		if(factoryCacheReady)
		{
			std::string cacheFilename;
			std::vector<uint8_t> cache;
			const bool cacheCaptured = getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					return device
						&& device->getModel() == md::MachineModel::Machinedrum
						&& device->captureFactoryFlashCachePersistence(
							cacheFilename, cache, cacheError);
				});
			cachePersisted = cacheCaptured
				&& md::Device::writeFactoryFlashCachePersistence(
					cacheFilename, cache, cacheError);
		}

		juce::String result;
		if(!rebootMachinedrum(result))
		{
			m_factoryInitializationStatus =
				"Factory samples are ready, but automatic reboot is waiting: " + result;
			// A state-save or storage operation can briefly win the transaction.
			// Avoid rebuilding a replacement four times per second while it does.
			startTimer(2000);
			return false;
		}

		if(state == State::FirmwareUpdateReady)
			m_factoryInitializationStatus = cachePersisted
				? "OS update installed; Machinedrum restarted into first-run preparation"
				: "OS update installed; Machinedrum restarted with project-owned flash";
		else if(cachePersisted)
			m_factoryInitializationStatus =
				"Factory samples initialized; Machinedrum rebooted automatically";
		else if(factoryCacheDisqualified)
			m_factoryInitializationStatus =
				"Machinedrum rebooted automatically; the project state was preserved";
		else
			m_factoryInitializationStatus =
				"Machinedrum rebooted automatically, but " + juce::String(cacheError);
		startTimer(1000);
		updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
			.withNonParameterStateChanged(true));
		std::fprintf(stderr, "[MD] factory initialization complete; rebooted in process\n");
		return true;
	}

	void AudioPluginAudioProcessor::timerCallback()
	{
		(void)serviceFactoryInitialization();
	}

	juce::String AudioPluginAudioProcessor::getPlusDrivePersistenceStatus() const
	{
		if(m_factoryInitializationStatus.isNotEmpty())
			return m_factoryInitializationStatus;
		return m_standalonePlusDrive
			? m_standalonePlusDrive->status()
			: "Project-owned +Drive; saved with the host project";
	}

	void AudioPluginAudioProcessor::initializeStandalonePlusDrivePersistence()
	{
		if(m_model != md::MachineModel::Machinedrum
			|| (wrapperType != juce::AudioProcessor::wrapperType_Standalone
				&& m_standalonePlusDriveFile == juce::File()))
			return;
		const auto file = m_standalonePlusDriveFile != juce::File()
			? m_standalonePlusDriveFile
			: juce::File(juce::String::fromUTF8(getDataFolder().c_str()))
				.getChildFile("nvram").getChildFile("md-plusdrive-standalone-v1.mdpd");
		m_standalonePlusDrive = std::make_unique<StandalonePlusDrivePersistence>(
			file, m_storageLoadMutex,
			[this](const bool _includeData,
				StandalonePlusDrivePersistence::Snapshot& _snapshot)
			{
				return capturePlusDrivePersistenceSnapshot(_includeData,
					_snapshot.hardwareEpoch, _snapshot.generation, _snapshot.dirty,
					_snapshot.data);
			},
			[this](const uint64_t _epoch, const uint64_t _generation)
			{
				acknowledgePlusDrivePersistence(_epoch, _generation);
			});
		const auto started = m_standalonePlusDrive->start();
		if(!started.hasInitialImage)
			return;
		const bool installed = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				return device && device->getModel() == md::MachineModel::Machinedrum
					&& device->getHardware().installStartupPlusDriveData(
						started.initialImage);
			});
		if(!installed)
			m_standalonePlusDrive->blockWrites(
				"The standalone +Drive checkpoint was valid but could not be installed");
	}

	bool AudioPluginAudioProcessor::capturePlusDrivePersistenceSnapshot(
		const bool _includeData, uint64_t& _epoch, uint64_t& _generation,
		bool& _dirty, std::vector<uint8_t>& _data)
	{
		return getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				auto& hardware = device->getHardware();
				_epoch = device->hardwareEpoch();
				_generation = hardware.plusDriveGeneration();
				_dirty = hardware.plusDriveDirty();
				if(_includeData)
					_data = hardware.copyPlusDriveData();
				return true;
			});
	}

	void AudioPluginAudioProcessor::acknowledgePlusDrivePersistence(
		const uint64_t _epoch, const uint64_t _generation)
	{
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			if(device && device->getModel() == md::MachineModel::Machinedrum
				&& device->hardwareEpoch() == _epoch)
				device->getHardware().markPlusDrivePersisted(_generation);
		});
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model)
		: AudioPluginAudioProcessor(_model, std::vector<uint8_t>{})
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		const bool _allowMcpServer)
		: AudioPluginAudioProcessor(_model, std::vector<uint8_t>{}, _allowMcpServer)
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		std::vector<uint8_t> _initialPatchRam, const bool _allowMcpServer) :
		AudioPluginAudioProcessor(_model, std::move(_initialPatchRam), _allowMcpServer, false)
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		EphemeralConfig _config, const bool _allowMcpServer) :
		AudioPluginAudioProcessor(_model, std::vector<uint8_t>{}, _allowMcpServer,
			true, std::move(_config.standalonePlusDriveFile),
			_config.isolateDeviceStorage, std::move(_config.romData),
			std::move(_config.romName))
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		std::vector<uint8_t> _initialPatchRam, const bool _allowMcpServer,
		const bool _ephemeralConfig, juce::File _standalonePlusDriveFile,
		const bool _isolateDeviceStorage, std::vector<uint8_t> _romData,
		std::string _romName) :
		Processor(createBusesProperties(),
			getOptions(_model, _ephemeralConfig), makeProcessorProperties(_model),
			_allowMcpServer, _ephemeralConfig
				? jucePluginEditorLib::Processor::ConfigMode::Ephemeral
				: jucePluginEditorLib::Processor::ConfigMode::Persistent)
		, m_model(_model)
		, m_initialPatchRam(std::move(_initialPatchRam))
		, m_standalonePlusDriveFile(std::move(_standalonePlusDriveFile))
		, m_isolateDeviceStorage(_isolateDeviceStorage)
		, m_romData(std::move(_romData))
		, m_romName(std::move(_romName))
	{
		// The hardware-width skins need more than the generic 100% default. Keep
		// the migration within a laptop desktop; the editor window restores this
		// configured scale after the standalone host's placeholder-size pass.
		constexpr auto scaleMigrationKey = "hardwarePanelScaleV4";
		constexpr auto readablePanelScale = 130;
		constexpr auto undersizedPanelScale = 120;
		if (!getConfig().getBoolValue(scaleMigrationKey, false))
		{
			if (!getConfig().containsKey("scale")
				|| getConfig().getDoubleValue("scale", 100) < undersizedPanelScale)
				getConfig().setValue("scale", readablePanelScale);

			getConfig().setValue(scaleMigrationKey, true);
			getConfig().saveIfNeeded();
		}

		getController();
		// UI-only and updater tests supply an isolated device configuration and
		// create the device explicitly when they need it. Latency initialization is
		// an audio-runtime concern; doing it here would eagerly boot a firmware-less
		// machine just to construct an editor.
		if(!_ephemeralConfig)
		{
			const auto latencyBlocks = getConfig().getIntValue("latencyBlocks",
				static_cast<int>(getPlugin().getLatencyBlocks()));
			Processor::setLatencyBlocks(latencyBlocks);
		}
		initializeStandalonePlusDrivePersistence();
		// Ephemeral processors drive factory initialization explicitly in tests.
		// Starting the production timer here can race a short-lived editor (and
		// pointlessly attempts ROM discovery in UI-only tests).
		if(m_model == md::MachineModel::Machinedrum && !_ephemeralConfig)
			startTimer(250);
	}

	juce::AudioProcessor::BusesProperties AudioPluginAudioProcessor::createBusesProperties()
	{
		auto buses = BusesProperties()
			.withInput("Input A/B", juce::AudioChannelSet::stereo(), true);

		// JUCE's Standalone wrapper disables every non-main bus after construction.
		// Use one adaptive physical-output bus there so its audio-device dialog can
		// expose all six outputs. Plug-in wrappers retain named stereo buses.
		if(juce::PluginHostType::getPluginLoadedAs()
			== juce::AudioProcessor::wrapperType_Standalone)
			return buses.withOutput("Outputs A-F",
				juce::AudioChannelSet::discreteChannels(6), true);

		return buses
			.withOutput("Main A/B", juce::AudioChannelSet::stereo(), true)
			.withOutput("Out C/D", juce::AudioChannelSet::stereo(), false)
			.withOutput("Out E/F", juce::AudioChannelSet::stereo(), false);
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		stopTimer();
		destroyEditorState();
		if(m_standalonePlusDrive)
			m_standalonePlusDrive->stop();
		m_standalonePlusDrive.reset();
	}

	bool AudioPluginAudioProcessor::isBusesLayoutSupported(
		const BusesLayout& _layout) const
	{
		if(_layout.inputBuses.size() != 1
			|| _layout.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
			return false;

		if(_layout.outputBuses.size() == 1)
		{
			const auto channels = _layout.getMainOutputChannelSet().size();
			return channels == 2 || channels == 4 || channels == 6;
		}

		if(_layout.outputBuses.size() != 3
			|| _layout.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
			return false;

		bool precedingOutputEnabled = true;
		for(int bus = 1; bus < _layout.outputBuses.size(); ++bus)
		{
			const auto channels = _layout.getChannelSet(false, bus);
			if(channels == juce::AudioChannelSet::disabled())
			{
				precedingOutputEnabled = false;
				continue;
			}
			if(!precedingOutputEnabled || channels != juce::AudioChannelSet::stereo())
				return false;
		}
		return true;
	}

	jucePluginEditorLib::PluginEditorState* AudioPluginAudioProcessor::createEditorState()
	{
		return new PluginEditorState(*this);
	}

	synthLib::Device* AudioPluginAudioProcessor::createDevice()
	{
		synthLib::DeviceCreateParams params;
		params.customData = md::deviceCustomData(m_model);
		if(!m_isolateDeviceStorage)
			params.homePath = getDataFolder();
		params.romData = m_romData;
		params.romName = m_romName;
		auto* d = new md::Device(params, m_initialPatchRam);
		if(!d->isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing,
				std::string("A ") + productName(m_model) + " firmware rom (8 MB .bin) is required, but was not found.");
		return d;
	}

	void AudioPluginAudioProcessor::getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const
	{
		Processor::getRemoteDeviceParams(_params);
		_params.customData = md::deviceCustomData(m_model);

		auto rom = md::RomLoader::findROM(m_model);

		if(rom.isValid())
		{
			_params.romData.assign(rom.data().begin(), rom.data().end());
			_params.romName = rom.getFilename();
		}
	}

	pluginLib::Controller* AudioPluginAudioProcessor::createController()
	{
		return new mdJucePlugin::Controller(*this);
	}
}
