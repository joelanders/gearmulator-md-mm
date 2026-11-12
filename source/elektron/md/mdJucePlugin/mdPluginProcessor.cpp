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
#include "mdLib/mdpanel.h"

#include "synthLib/deviceException.h"

#include "baseLib/binarystream.h"

#include "juce_audio_utils/juce_audio_utils.h"
#include "juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h"

#include <memory>
#include <utility>

namespace
{
	synthLib::PerformanceReport::Context panelEventDetails(const synthLib::RealtimeEvent& _event)
	{
		using Kind = synthLib::RealtimeEventKind;
		if(_event.kind == Kind::HostTransport) return {};
		const auto model = static_cast<md::MachineModel>(_event.model);
		if(_event.command >= 0x20 && _event.command <= 0x25)
		{
			std::string down, up;
			for(unsigned i = 0; i <= static_cast<unsigned>(md::PanelControl::ClassicExtended); ++i)
			{
				const auto control = static_cast<md::PanelControl>(i);
				const auto packet = md::panelPacket(model, control);
				if(!packet || packet->row != _event.command) continue;
				auto& names = (_event.argument & packet->mask) ? down : up;
				if(!names.empty()) names += '|';
				names += md::panelControlName(control);
			}
			return {{"panelKind", "row_state"}, {"buttonsDown", down}, {"buttonsUp", up}};
		}
		for(unsigned i = 0; i <= static_cast<unsigned>(md::PanelEncoder::SoundSelection); ++i)
		{
			const auto encoder = static_cast<md::PanelEncoder>(i);
			const auto command = md::panelEncoderCommand(model, encoder);
			if(command && *command == _event.command)
				return {{"panelKind", "encoder"}, {"encoder", md::panelEncoderName(encoder)},
					{"steps", std::to_string(_event.argument < 128 ? static_cast<int>(_event.argument)
						: static_cast<int>(_event.argument) - 256)}};
		}
		return {{"panelKind", "unmapped_packet"}};
	}

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
			compiled.lv2Uri, compiled.binaryData, dataFolderName(_model),
			{0}, {0, 2, 4}
		};
	}

}

namespace mdJucePlugin
{
	void AudioPluginAudioProcessor::saveChunkData(baseLib::BinaryStream& _stream)
	{
		jucePluginEditorLib::Processor::saveChunkData(_stream);
		if(m_model == md::MachineModel::Machinedrum)
		{
			baseLib::ChunkWriter chunk(_stream, "RAMF", 1);
			_stream.write(static_cast<uint8_t>(getRamRecordingMode()));
		}
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
		_reader.add("RAMF", 1, [this](baseLib::BinaryStream& _stream, uint32_t)
		{
			m_ramRecordingModeChunkSeen = true;
			const auto mode = static_cast<md::RamRecordingMode>(_stream.read<uint8_t>());
			if(mode == md::RamRecordingMode::Original
				|| mode == md::RamRecordingMode::CompleteTail)
				setRamRecordingMode(mode);
		});
	}

	bool AudioPluginAudioProcessor::loadCustomData(const std::vector<uint8_t>& _sourceBuffer)
	{
		// The standalone checkpoint, not JUCE's filterState, owns the standalone
		// +Drive once it exists. Preserve it across the filterState load.
		std::unique_lock<std::mutex> operationLock;
		uint64_t preservedEpoch = 0;
		uint64_t preservedGeneration = 0;
		bool preservedDirty = false;
		std::vector<uint8_t> preservedDrive;
		bool preserveStandaloneDrive = false;
		if(m_standalonePlusDrive)
		{
			operationLock = std::unique_lock<std::mutex>(m_storageLoadMutex);
			preserveStandaloneDrive = m_standalonePlusDrive->authoritative()
				&& capturePlusDrivePersistenceSnapshot(true, preservedEpoch,
					preservedGeneration, preservedDirty, preservedDrive);
		}

		const auto previous = getRamRecordingMode();
		m_ramRecordingModeChunkSeen = false;
		const bool result = jucePluginEditorLib::Processor::loadCustomData(_sourceBuffer);
		if(!result)
		{
			setRamRecordingMode(previous);
			return false;
		}
		if(m_model == md::MachineModel::Machinedrum && !m_ramRecordingModeChunkSeen)
			setRamRecordingMode(md::RamRecordingMode::Original);
		if(!m_standalonePlusDrive)
			return true;

		if(preserveStandaloneDrive)
		{
			bool missedBoot = false;
			const bool restored = getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					if(!device || !device->installPlusDrive(preservedDrive, preservedDirty))
						return false;
					missedBoot = device->getHardware().isPlusDriveConnected()
						&& device->getHardware().hasFirmwareStarted();
					return true;
				});
			if(!restored)
			{
				m_standalonePlusDrive->blockWrites(
					"Standalone state loaded, but its authoritative +Drive could not be restored");
				return false;
			}
			if(preservedDirty)
				m_standalonePlusDrive->requestFlush();
			operationLock.unlock();
			if(missedBoot)
				rebootForPlusDrive();
		}
		else
		{
			// First launch after upgrading from alpha.7/alpha.8: when no dedicated
			// checkpoint exists, adopt a +Drive carried by filterState exactly once.
			m_standalonePlusDrive->requestFlush(true);
		}
		return true;
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
		bool stateRestorePending = false;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device);
				device && device->getModel() == md::MachineModel::Monomachine)
			{
				stateRestorePending = device->isProjectStateRestorePending();
				if(!stateRestorePending)
					preparationContext = device->getPreparationContext();
			}
		});
		if(!preparationContext)
		{
			_result = stateRestorePending
				? "Storage was not changed. Finish loading the project state first"
				: "Storage was not changed. The machine is not using a local device";
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
				if(!device || device->getPreparationContext() != preparationContext
					|| device->isProjectStateRestorePending())
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
							|| device->getPreparationContext() != preparationContext
							|| device->isProjectStateRestorePending())
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

	bool AudioPluginAudioProcessor::rebootWithPlusDrive(
		const std::vector<uint8_t>* const _replacement, const juce::String& _operation,
		juce::String& _result)
	{
		_result.clear();
		if(m_model != md::MachineModel::Machinedrum)
		{
			_result = "+Drive operations are only supported by Gearmulator MD";
			return false;
		}
		if(_replacement && (_replacement->size() > md::g_plusDriveMaxSerializedBytes
			|| _replacement->empty() || !md::PlusDrive::validateStorage(*_replacement)))
		{
			_result = "The replacement is not a valid bounded sparse +Drive image";
			return false;
		}
		if(_replacement && m_standalonePlusDrive && !m_standalonePlusDrive->ownsWriter())
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
		bool busy = false;
		std::vector<uint8_t> originalState;
		md::FactoryFlashSnapshot factoryFlash;
		std::shared_ptr<const md::Device::PreparationContext> preparationContext;
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum
					|| !device->isValid())
					return false;
				// A pending restore or first-run initialization is followed by its own
				// reboot. Replacing the machine underneath it could lose either.
				if(device->isProjectStateRestorePending()
					|| device->getHardware().isFactoryFlashInitializationExpected())
				{
					busy = true;
					return false;
				}
				liveDevice = device;
				liveEpoch = device->hardwareEpoch();
				preparationContext = device->getPreparationContext();
				(void)device->getHardware().copyFactoryFlashSnapshot(factoryFlash);
				return device->getState(originalState, synthLib::StateTypeGlobal);
			});
		if(!captured || !preparationContext)
		{
			_result = _operation + " was not applied. " + (busy
				? "Wait until the Machinedrum has finished starting or loading the project"
				: "The local Machinedrum state could not be captured");
			return false;
		}

		// All encoding, validation and Hardware construction stays outside the
		// Plugin lock. The live machine keeps running meanwhile.
		auto replacementState = originalState;
		if(_replacement && !md::replacePlusDriveState(replacementState, *_replacement))
		{
			_result = _operation + " was not applied. The current project state could not accept the +Drive image";
			return false;
		}
		std::string prepareError;
		auto prepared = md::Device::prepareState(preparationContext, replacementState,
			synthLib::StateTypeGlobal, factoryFlash, &prepareError);
		if(!prepared)
		{
			_result = _operation + " was not applied. " + juce::String(prepareError);
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
		// A successful commit leaves the retired Hardware here. Release it only
		// after the Plugin lock has been dropped.
		prepared.reset();
		if(!committed)
		{
			_result = _operation + " was not applied. " + commitError;
			return false;
		}
		if(hasController())
			getController().onStateLoaded();
		updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
			.withNonParameterStateChanged(true));
		if(_replacement && m_standalonePlusDrive)
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
		return rebootWithPlusDrive(&replacement,
			"+Drive image '" + _source.getFileName() + "'", _result);
	}

	bool AudioPluginAudioProcessor::exportPlusDriveImage(const juce::File& _target,
		juce::String& _result)
	{
		_result.clear();
		if(m_model != md::MachineModel::Machinedrum)
		{
			_result = "+Drive images are only supported by Gearmulator MD";
			return false;
		}
		std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
		if(!operationLock.owns_lock())
		{
			_result = "Another machine-storage operation is already running";
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
		if(data.empty())
		{
			_result = "No +Drive is connected. Create or import one first";
			return false;
		}
		if(!storageImage::writePlusDriveAtomically(_target, data, _result))
			return false;
		_result = "+Drive image exported atomically to " + _target.getFullPathName();
		return true;
	}

	bool AudioPluginAudioProcessor::resetPlusDrive(juce::String& _result)
	{
		const auto blank = md::PlusDrive().copyStorage();
		return rebootWithPlusDrive(&blank, "Blank +Drive", _result);
	}

	bool AudioPluginAudioProcessor::rebootMachinedrum(juce::String& _result)
	{
		return rebootWithPlusDrive(nullptr, "Reboot", _result);
	}

	juce::String AudioPluginAudioProcessor::getPlusDrivePersistenceStatus() const
	{
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
				return device && device->installPlusDrive(started.initialImage, false);
			});
		if(!installed)
		{
			m_standalonePlusDrive->blockWrites(
				"The standalone +Drive checkpoint was valid but could not be installed");
			return;
		}
		const bool missedBoot = getPlugin().withDeviceLocked(
			[](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				return device && device->getHardware().isPlusDriveConnected()
					&& device->getHardware().hasFirmwareStarted();
			});
		if(missedBoot)
			rebootForPlusDrive();
	}

	void AudioPluginAudioProcessor::rebootForPlusDrive()
	{
		juce::String result;
		if(!rebootMachinedrum(result))
			std::fprintf(stderr, "[MD] +Drive reboot failed: %s\n", result.toRawUTF8());
	}

	bool AudioPluginAudioProcessor::capturePlusDrivePersistenceSnapshot(
		const bool _includeData, uint64_t& _epoch, uint64_t& _generation,
		bool& _dirty, std::vector<uint8_t>& _data)
	{
		return getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				// Nothing to checkpoint while no +Drive is connected, or while a
				// restore has not yet exposed its final machine.
				if(!device || device->getModel() != md::MachineModel::Machinedrum
					|| device->isProjectStateRestorePending()
					|| !device->getHardware().hasPlusDrive())
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
		AudioPluginAudioProcessor(_model, std::vector<uint8_t>{}, _allowMcpServer, true,
			std::move(_config.deviceHomePath), std::move(_config.standalonePlusDriveFile))
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		std::vector<uint8_t> _initialPatchRam, const bool _allowMcpServer,
		const bool _ephemeralConfig,
		std::optional<std::string> _deviceHomePath,
		juce::File _standalonePlusDriveFile) :
		Processor(createBusesProperties(),
			getOptions(_model, _ephemeralConfig), makeProcessorProperties(_model),
			_allowMcpServer, _ephemeralConfig
				? jucePluginEditorLib::Processor::ConfigMode::Ephemeral
				: jucePluginEditorLib::Processor::ConfigMode::Persistent)
		, m_model(_model)
		, m_initialPatchRam(std::move(_initialPatchRam))
		, m_deviceHomePath(std::move(_deviceHomePath))
		, m_standalonePlusDriveFile(std::move(_standalonePlusDriveFile))
	{
		if(m_model == md::MachineModel::Machinedrum)
			m_ramRecordingMode.store(
				static_cast<uint8_t>(md::RamRecordingMode::CompleteTail),
				std::memory_order_relaxed);
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
		setRamRecordingMode(getRamRecordingMode());
		const auto latencyBlocks = getConfig().getIntValue("latencyBlocks", static_cast<int>(getPlugin().getLatencyBlocks()));
		Processor::setLatencyBlocks(latencyBlocks);
		initializeStandalonePlusDrivePersistence();
		m_startupDiagnosticsEnabled = !_ephemeralConfig
			&& juce::JUCEApplicationBase::isStandaloneApp();
		if(m_startupDiagnosticsEnabled)
		{
			const auto folder = performanceDiagnosticsFolder();
			if(folder.createDirectory().wasOk())
			{
				m_startupDiagnosticsFile = folder.getChildFile("standalone-startup-last.log");
				m_startupDiagnosticsFile.deleteFile();
				m_startupDiagnosticsStartMilliseconds
					= juce::Time::getMillisecondCounterHiRes();
				m_startupDiagnosticsFile.appendText(
					"time=" + juce::Time::getCurrentTime().toISO8601(true)
					+ " model=" + productName(m_model)
					+ " revision=" + juce::String(MDMM_DIAGNOSTICS_REVISION) + "\n");
			}
			else
				m_startupDiagnosticsEnabled = false;
		}
		if(m_model == md::MachineModel::Machinedrum || m_startupDiagnosticsEnabled)
			startTimer(250);
		m_performanceReport = std::make_unique<synthLib::PerformanceReport>(
			getPlugin().getRealtimeInstrumentation(), panelEventDetails);
		// The environment switch is also useful in hosts without an open editor.
		if(getPlugin().getRealtimeInstrumentation().isEnabled())
			setPerformanceDiagnosticsEnabled(true);
	}

	juce::AudioProcessor::BusesProperties AudioPluginAudioProcessor::createBusesProperties()
	{
		// Match the established Gearmulator bus model: plug-in hosts may enable the
		// two auxiliary stereo buses, while JUCE Standalone deliberately disables
		// non-main buses and opens the physical device as stereo. Keeping one stable
		// topology also removes wrapper-dependent construction from the processor.
		return BusesProperties()
			.withInput("Input A/B", juce::AudioChannelSet::stereo(), true)
			.withOutput("Main A/B", juce::AudioChannelSet::stereo(), true)
			.withOutput("Out C/D", juce::AudioChannelSet::stereo(), false)
			.withOutput("Out E/F", juce::AudioChannelSet::stereo(), false);
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		stopTimer();
		// Final standalone checkpoint flush while the device still exists.
		if(m_standalonePlusDrive)
			m_standalonePlusDrive->stop();
		m_standalonePlusDrive.reset();
		m_performanceReport.reset();
		destroyEditorState();
	}

	juce::File AudioPluginAudioProcessor::performanceDiagnosticsFolder() const
	{
		return juce::File(getDataFolder()).getChildFile("logs");
	}

	bool AudioPluginAudioProcessor::performanceDiagnosticsActive() const
	{
		if(!m_performanceReport) return false;
		const auto status = m_performanceReport->status();
		return status == synthLib::PerformanceReport::Status::Starting
			|| status == synthLib::PerformanceReport::Status::Recording;
	}

	std::string AudioPluginAudioProcessor::performanceDiagnosticsStatus() const
	{
		if(m_performanceFolderError) return "Could not create the logs folder.";
		if(!m_performanceReport) return "Off";
		using Status = synthLib::PerformanceReport::Status;
		switch(m_performanceReport->status())
		{
		case Status::Starting: return "Starting performance capture...";
		case Status::Recording: return "Recording performance diagnostics (maximum 10 minutes / 8 MiB).";
		case Status::Stopped: return "Capture saved. Open the logs folder to share the report.";
		case Status::LimitReached: return "Capture limit reached. Report saved; recording is off.";
		case Status::Error: return "Could not write the performance report. Check free space and folder permissions.";
		case Status::Idle: return "Off";
		}
		return "Off";
	}

	void AudioPluginAudioProcessor::setPerformanceDiagnosticsEnabled(const bool _enabled)
	{
		if(!m_performanceReport) return;
		if(!_enabled) { m_performanceReport->stop(); return; }
		if(performanceDiagnosticsActive()) return;
		m_performanceFolderError = performanceDiagnosticsFolder().createDirectory().failed();
		if(m_performanceFolderError)
		{
			getPlugin().getRealtimeInstrumentation().setEnabled(false);
			return;
		}
		m_performanceReportFile = performanceDiagnosticsFolder().getChildFile(
			"performance-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S")
			+ "-" + juce::Uuid().toString() + ".jsonl");
		synthLib::PerformanceReport::Context context{
			{"product", getProductName()},
			{"version", JucePlugin_VersionString},
			{"revision", MDMM_DIAGNOSTICS_REVISION},
			{"started", juce::Time::getCurrentTime().toISO8601(true).toStdString()},
			{"host", juce::PluginHostType().getHostDescription()},
			{"format", juce::AudioProcessor::getWrapperTypeDescription(wrapperType)},
			{"os", juce::SystemStats::getOperatingSystemName().toStdString()},
			{"cpu", juce::SystemStats::getCpuModel().toStdString()},
			{"cpu_vendor", juce::SystemStats::getCpuVendor().toStdString()},
			{"logical_cpus", std::to_string(juce::SystemStats::getNumCpus())},
			{"cpu_mhz", std::to_string(juce::SystemStats::getCpuSpeedInMegahertz())},
			#if JUCE_ARM
			{"architecture", "arm"},
#else
			{"architecture", "x86"},
#endif
			{"pointer_bits", std::to_string(sizeof(void*) * 8)},
			{"resampler_modes", "0=Legacy,1=MameHq,2=MameLofi"},
			{"notes", "Nested timings are inclusive. JIT values are counts, not compilation durations. Deadline overruns are estimates, not host xrun reports. MIDI counts contain no payload."}
		};
		m_performanceReport->start(m_performanceReportFile.getFullPathName().toStdString(), std::move(context));
	}

	bool AudioPluginAudioProcessor::isBusesLayoutSupported(
		const BusesLayout& _layout) const
	{
		if(_layout.inputBuses.size() != 1)
			return false;
		const auto input = _layout.getMainInputChannelSet();
		if(input != juce::AudioChannelSet::disabled()
			&& input != juce::AudioChannelSet::stereo())
			return false;

		if(_layout.outputBuses.size() != 3
			|| _layout.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
			return false;

		for(int bus = 1; bus < _layout.outputBuses.size(); ++bus)
		{
			const auto channels = _layout.getChannelSet(false, bus);
			if(channels != juce::AudioChannelSet::disabled()
				&& channels != juce::AudioChannelSet::stereo())
				return false;
		}
		return true;
	}
	bool AudioPluginAudioProcessor::serviceFactoryInitialization()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return false;

		enum class State { Waiting, Ready, NotNeeded };
		auto state = State::NotNeeded;
		md::Device* liveDevice = nullptr;
		uint64_t liveEpoch = 0;
		std::vector<uint8_t> originalState;
		std::string cacheFilename;
		md::FactoryFlashSnapshot factoryFlash;
		std::string cacheError;
		std::shared_ptr<const md::Device::PreparationContext> preparationContext;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			if(!device || device->getModel() != md::MachineModel::Machinedrum
				|| !device->isValid())
				return;
			if(device->isProjectStateRestorePending())
			{
				state = State::Waiting;
				return;
			}
			auto& hardware = device->getHardware();
			if(!hardware.isFactoryFlashInitializationExpected())
				return;
			state = hardware.isFactoryFlashReadyForReboot()
				? State::Ready : State::Waiting;
			if(state != State::Ready)
				return;

			liveDevice = device;
			liveEpoch = device->hardwareEpoch();
			preparationContext = device->getPreparationContext();
			if(hardware.isFactoryFlashCacheReady())
				(void)device->captureFactoryFlashCachePersistence(cacheFilename,
					factoryFlash, cacheError);
			if(!device->getState(originalState, synthLib::StateTypeGlobal))
				state = State::Waiting;
		});

		if(state == State::NotNeeded)
		{
			startTimer(1000);
			return false;
		}
		if(state != State::Ready || !preparationContext || !liveDevice)
		{
			startTimer(250);
			return false;
		}

		// Full-image cache encoding, filesystem promotion, and replacement
		// construction stay outside synthLib::Plugin's process/device lock.
		if(!md::Device::materializeFactoryFlashCache(factoryFlash,
			preparationContext, cacheError))
			std::fprintf(stderr, "[MD] %s\n", cacheError.c_str());
		else if(!factoryFlash.cache.empty()
			&& !md::Device::writeFactoryFlashCachePersistence(cacheFilename,
				factoryFlash.cache, cacheError))
			std::fprintf(stderr, "[MD] %s\n", cacheError.c_str());

		auto prepared = md::Device::prepareState(preparationContext, originalState,
			synthLib::StateTypeGlobal, factoryFlash);
		if(!prepared)
		{
			startTimer(2000);
			return false;
		}

		const bool committed = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(device != liveDevice || !device
					|| device->hardwareEpoch() != liveEpoch)
					return false;
				std::vector<uint8_t> currentState;
				if(!device->getState(currentState, synthLib::StateTypeGlobal)
					|| currentState != originalState)
					return false;
				return device->commitPreparedState(*prepared);
			});
		// A successful commit leaves the retired Hardware here. Release it only
		// after the process/device lock has been dropped.
		prepared.reset();
		if(!committed)
		{
			startTimer(2000);
			return false;
		}
		if(hasController())
			getController().onStateLoaded();
		updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
			.withNonParameterStateChanged(true));
		std::fprintf(stderr,
			"[MD] factory flash preparation complete; rebooted in process\n");
		startTimer(1000);
		return true;
	}

	bool AudioPluginAudioProcessor::serviceDeferredStateRestore()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return false;
		md::Device* liveDevice = nullptr;
		uint64_t liveEpoch = 0;
		uint64_t generation = 0;
		std::unique_ptr<md::Device::PreparedState> validated;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			if(!device || device->getModel() != md::MachineModel::Machinedrum)
				return;
			liveDevice = device;
			liveEpoch = device->hardwareEpoch();
			validated = device->takeFinishedDeferredState(generation);
		});
		if(!validated)
			return false;

		// A failed baseline check never touched the live Hardware. Drop only the
		// isolated candidate and leave the current machine running.
		auto prepared = md::Device::makeDeferredStateReboot(*validated);
		if(!prepared)
		{
			const std::string error =
				"The deferred project state failed validation; the live machine was not changed.";
			getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					if(device == liveDevice && device
						&& device->hardwareEpoch() == liveEpoch)
						(void)device->rejectDeferredStateRestore(generation, error);
				});
			validated.reset();
			return false;
		}

		std::string cacheFilename;
		md::FactoryFlashSnapshot factoryFlash;
		std::string cacheError;
		std::shared_ptr<const md::Device::PreparationContext> preparationContext;
		const bool committed = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(device != liveDevice || !device
					|| device->hardwareEpoch() != liveEpoch
					|| device->deferredStateGeneration() != generation)
					return false;
				if(!device->commitDeferredStateRestore(*prepared, generation))
					return false;
				preparationContext = device->getPreparationContext();
				(void)device->captureFactoryFlashCachePersistence(cacheFilename,
					factoryFlash, cacheError);
				return true;
			});
		prepared.reset();
		validated.reset();
		if(!committed)
			return false;
		if(!md::Device::materializeFactoryFlashCache(factoryFlash,
			preparationContext, cacheError))
			std::fprintf(stderr, "[MD] %s\n", cacheError.c_str());
		else if(!factoryFlash.cache.empty()
			&& !md::Device::writeFactoryFlashCachePersistence(cacheFilename,
				factoryFlash.cache, cacheError))
			std::fprintf(stderr, "[MD] %s\n", cacheError.c_str());
		if(hasController())
			getController().onStateLoaded();
		updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
			.withNonParameterStateChanged(true));
		std::fprintf(stderr,
			"[MD] deferred project state validated and rebooted in process\n");
		return true;
	}

	bool AudioPluginAudioProcessor::serviceStateRestoreFailure()
	{
		uint64_t generation = 0;
		std::string error;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			if(!device || device->projectStateRestoreStatus()
				!= md::Device::ProjectStateRestoreStatus::Failed)
				return;
			generation = device->deferredStateGeneration();
			error = device->projectStateRestoreError();
		});
		if(error.empty() || generation == m_reportedRestoreFailureGeneration)
			return false;
		m_reportedRestoreFailureGeneration = generation;
		reportProjectStateRestoreFailure(error);
		return true;
	}

	bool AudioPluginAudioProcessor::serviceProjectStateRestore()
	{
		if(serviceDeferredStateRestore())
			return true;
		return serviceStateRestoreFailure();
	}

	std::string AudioPluginAudioProcessor::getProjectStateRestoreError()
	{
		return getPlugin().withDeviceLocked([](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			return device ? device->projectStateRestoreError() : std::string{};
		});
	}

	void AudioPluginAudioProcessor::reportProjectStateRestoreFailure(
		const std::string& _error)
	{
		std::fprintf(stderr, "[MD] %s\n", _error.c_str());
		updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
			.withNonParameterStateChanged(true));
		if(getActiveEditor())
			juce::NativeMessageBox::showMessageBoxAsync(
				juce::MessageBoxIconType::WarningIcon,
				std::string(productName(m_model)) + " state restore", _error);
	}

	void AudioPluginAudioProcessor::recordStandaloneStartupDiagnostics()
	{
		if(!m_startupDiagnosticsEnabled)
			return;
		const auto elapsed = juce::Time::getMillisecondCounterHiRes()
			- m_startupDiagnosticsStartMilliseconds;
		if(elapsed > 20000.0)
		{
			m_startupDiagnosticsEnabled = false;
			return;
		}

		auto* const holder = juce::StandalonePluginHolder::getInstance();
		auto* const audioDevice = holder
			? holder->deviceManager.getCurrentAudioDevice() : nullptr;
		uint64_t cycles = 0;
		uint64_t epoch = 0;
		uint32_t pixels = 0;
		uint32_t panelBytes = 0;
		uint32_t tileWrites = 0;
		getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
		{
			auto* const device = dynamic_cast<md::Device*>(_device);
			if(!device)
				return;
			const auto panel = device->getHardware().getFrontPanelSnapshot();
			cycles = device->getHardware().hostCurrentCycle();
			epoch = device->hardwareEpoch();
			pixels = panel.countLitPixels();
			panelBytes = panel.getByteCount();
			tileWrites = panel.getTileWriteCount();
		});

		juce::String line;
		line << "ms=" << static_cast<int64_t>(elapsed)
			<< " device=" << static_cast<int>(audioDevice != nullptr)
			<< " playing=" << static_cast<int>(audioDevice && audioDevice->isPlaying())
			<< " cycles=" << static_cast<int64_t>(cycles)
			<< " epoch=" << static_cast<int64_t>(epoch)
			<< " pixels=" << static_cast<int64_t>(pixels)
			<< " panelBytes=" << static_cast<int64_t>(panelBytes)
			<< " tileWrites=" << static_cast<int64_t>(tileWrites)
			<< " editor=" << static_cast<int>(getActiveEditor() != nullptr) << "\n";
		m_startupDiagnosticsFile.appendText(line);
	}

	void AudioPluginAudioProcessor::timerCallback()
	{
		recordStandaloneStartupDiagnostics();
		if(serviceProjectStateRestore())
			return;
		(void)serviceFactoryInitialization();
	}

	jucePluginEditorLib::PluginEditorState* AudioPluginAudioProcessor::createEditorState()
	{
		return new PluginEditorState(*this);
	}

	synthLib::Device* AudioPluginAudioProcessor::createDevice()
	{
		synthLib::DeviceCreateParams params;
		params.customData = md::deviceCustomData(m_model);
		params.homePath = m_deviceHomePath ? *m_deviceHomePath : getDataFolder();
		auto d = std::make_unique<md::Device>(params, m_initialPatchRam);
		if(!d->isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing,
				std::string("A ") + productName(m_model) +
				" firmware rom (8 MB .bin) is required, but was not found.\n\n"
				"Do NOT discuss firmware or ROMs in Discord. "
				"Do not request or share files or download links, "
				"or ask for help obtaining or installing firmware.");
		d->setRamRecordingMode(getRamRecordingMode());
		return d.release();
	}

	void AudioPluginAudioProcessor::setRamRecordingMode(md::RamRecordingMode _mode)
	{
		if(m_model != md::MachineModel::Machinedrum)
			_mode = md::RamRecordingMode::Original;
		m_ramRecordingMode.store(static_cast<uint8_t>(_mode), std::memory_order_relaxed);
		getPlugin().withDeviceLocked([_mode](synthLib::Device* const _device)
		{
			if(auto* const device = dynamic_cast<md::Device*>(_device))
				device->setRamRecordingMode(_mode);
		});
	}

	bool AudioPluginAudioProcessor::isRamRecordingModeAvailable()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return false;
		return getPlugin().withDeviceLocked([](synthLib::Device* const _device)
		{
			const auto* const device = dynamic_cast<const md::Device*>(_device);
			return device && device->supportsRamRecordingMode();
		});
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
