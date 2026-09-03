#include "mdPluginProcessor.h"

#include "mdController.h"
#include "mdPluginEditorState.h"
#include "mdStorageImage.h"

// ReSharper disable once CppUnusedIncludeDirective
#include "BinaryData.h"
#include "jucePluginLib/processorPropertiesInit.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"

#include "synthLib/deviceException.h"

#include "baseLib/binarystream.h"

#include <memory>
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
			std::move(_config.deviceHomePath))
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		std::vector<uint8_t> _initialPatchRam, const bool _allowMcpServer,
		const bool _ephemeralConfig,
		std::optional<std::string> _deviceHomePath) :
		Processor(createBusesProperties(),
			getOptions(_model, _ephemeralConfig), makeProcessorProperties(_model),
			_allowMcpServer, _ephemeralConfig
				? jucePluginEditorLib::Processor::ConfigMode::Ephemeral
				: jucePluginEditorLib::Processor::ConfigMode::Persistent)
		, m_model(_model)
		, m_initialPatchRam(std::move(_initialPatchRam))
		, m_deviceHomePath(std::move(_deviceHomePath))
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
		const auto latencyBlocks = getConfig().getIntValue("latencyBlocks", static_cast<int>(getPlugin().getLatencyBlocks()));
		Processor::setLatencyBlocks(latencyBlocks);
		if(m_model == md::MachineModel::Machinedrum)
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

		if(_layout.outputBuses.size() == 1)
		{
			const auto channels = _layout.getMainOutputChannelSet().size();
			return channels == 2 || channels == 4 || channels == 6;
		}

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

	void AudioPluginAudioProcessor::timerCallback()
	{
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
				std::string("A ") + productName(m_model) + " firmware rom (8 MB .bin) is required, but was not found.");
		return d.release();
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
