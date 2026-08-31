#include "mdPluginProcessor.h"

#include "mdController.h"
#include "mdPluginEditorState.h"
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

#include <utility>
#include <chrono>
#include <unordered_map>

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

	std::mutex g_plusDriveMirrorOwnersMutex;
	std::unordered_map<std::string, const mdJucePlugin::AudioPluginAudioProcessor*>
		g_plusDriveMirrorOwners;

	bool claimPlusDriveMirror(const std::string& _path,
		const mdJucePlugin::AudioPluginAudioProcessor* const _owner)
	{
		std::lock_guard lock(g_plusDriveMirrorOwnersMutex);
		if(const auto existing = g_plusDriveMirrorOwners.find(_path);
			existing != g_plusDriveMirrorOwners.end() && existing->second != _owner)
			return false;
		g_plusDriveMirrorOwners[_path] = _owner;
		return true;
	}

	void releasePlusDriveMirror(const std::string& _path,
		const mdJucePlugin::AudioPluginAudioProcessor* const _owner)
	{
		std::lock_guard lock(g_plusDriveMirrorOwnersMutex);
		const auto existing = g_plusDriveMirrorOwners.find(_path);
		if(existing != g_plusDriveMirrorOwners.end() && existing->second == _owner)
			g_plusDriveMirrorOwners.erase(existing);
	}

	void releasePlusDriveMirror(
		const mdJucePlugin::AudioPluginAudioProcessor* const _owner)
	{
		std::lock_guard lock(g_plusDriveMirrorOwnersMutex);
		for(auto it = g_plusDriveMirrorOwners.begin();
			it != g_plusDriveMirrorOwners.end();)
		{
			if(it->second == _owner)
				it = g_plusDriveMirrorOwners.erase(it);
			else
				++it;
		}
	}

	juce::String plusDriveMirrorLockName(const std::string& _path)
	{
		return "gearmulator-md-plusdrive-"
			+ juce::String::toHexString(
				juce::String::fromUTF8(_path.c_str()).hashCode64());
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
	auto AudioPluginAudioProcessor::makeBuses(const md::MachineModel _model)
		-> BusesProperties
	{
		if(_model == md::MachineModel::Machinedrum)
			return BusesProperties()
				.withInput("Input", juce::AudioChannelSet::stereo(), true)
				.withOutput("Out", juce::AudioChannelSet::stereo(), true);
		return BusesProperties()
			.withOutput("Out", juce::AudioChannelSet::stereo(), true);
	}

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
		const bool loaded = jucePluginEditorLib::Processor::loadCustomData(_sourceBuffer);
		if(loaded && m_model == md::MachineModel::Machinedrum)
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			if(!m_plusDriveAutoSavePath.empty())
				++m_plusDriveFlushRequest;
		}
		m_plusDrivePersistenceCv.notify_all();
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
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				liveDevice = device;
				liveEpoch = device->hardwareEpoch();
				preparationContext = device->getPreparationContext();
				factoryFlashCache = device->getHardware().copyFactoryFlashCache();
				return device->getState(originalState, synthLib::StateTypeGlobal);
			});
		if(!captured || !preparationContext)
		{
			_result = "The local Machinedrum state could not be captured";
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
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			if(!m_plusDriveAutoSavePath.empty())
				++m_plusDriveFlushRequest;
		}
		m_plusDrivePersistenceCv.notify_all();
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
		md::Device* capturedDevice = nullptr;
		uint64_t capturedEpoch = 0;
		uint64_t capturedGeneration = 0;
		std::vector<uint8_t> data;
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				capturedDevice = device;
				capturedEpoch = device->hardwareEpoch();
				capturedGeneration = device->getHardware().plusDriveGeneration();
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
		std::string mirrorPath;
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			mirrorPath = m_plusDriveAutoSavePath;
		}
		if(mirrorPath == _target.getFullPathName().toStdString())
			getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(device == capturedDevice && device
					&& device->hardwareEpoch() == capturedEpoch)
					device->getHardware().markPlusDrivePersisted(capturedGeneration);
			});
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
		const bool captured = getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return false;
				liveDevice = device;
				liveEpoch = device->hardwareEpoch();
				preparationContext = device->getPreparationContext();
				factoryFlashCache = device->getHardware().copyFactoryFlashCache();
				return device->getState(originalState, synthLib::StateTypeGlobal);
			});
		if(!captured || !preparationContext)
		{
			_result = "The project-owned Machinedrum state could not be captured";
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
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			if(!m_plusDriveAutoSavePath.empty())
				++m_plusDriveFlushRequest;
		}
		m_plusDrivePersistenceCv.notify_all();
		_result = "The Machinedrum rebooted with its project-owned +Drive";
		return true;
	}

	bool AudioPluginAudioProcessor::rebootDevice()
	{
		if(m_model != md::MachineModel::Machinedrum)
			return jucePluginEditorLib::Processor::rebootDevice();
		juce::String result;
		return rebootMachinedrum(result);
	}

	bool AudioPluginAudioProcessor::enablePlusDriveAutoSave(const juce::File& _target,
		juce::String& _result)
	{
		stopPlusDrivePersistenceThread();
		std::string oldPath;
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			oldPath = m_plusDriveAutoSavePath;
		}
		std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
		if(!operationLock.owns_lock())
		{
			if(!oldPath.empty())
				startPlusDrivePersistenceThread();
			_result = "Another machine-storage operation is already running";
			return false;
		}
		const auto targetPath = _target.getFullPathName().toStdString();
		const bool changingTarget = targetPath != oldPath;
		std::unique_ptr<juce::InterProcessLock> candidateLock;
		bool candidateEntered = false;
		if(changingTarget)
		{
			if(!claimPlusDriveMirror(targetPath, this))
			{
				if(!oldPath.empty())
					startPlusDrivePersistenceThread();
				_result = "Another running plug-in instance already owns this auto-save mirror";
				return false;
			}
			candidateLock = std::make_unique<juce::InterProcessLock>(
				plusDriveMirrorLockName(targetPath));
			candidateEntered = candidateLock->enter(0);
			if(!candidateEntered)
			{
				releasePlusDriveMirror(targetPath, this);
				if(!oldPath.empty())
					startPlusDrivePersistenceThread();
				_result = "Another process already owns this auto-save mirror";
				return false;
			}
		}
		if(!exportPlusDriveImageUnlocked(_target, _result))
		{
			if(changingTarget)
			{
				releasePlusDriveMirror(targetPath, this);
				candidateLock->exit();
			}
			if(!oldPath.empty())
				startPlusDrivePersistenceThread();
			return false;
		}
		if(changingTarget)
		{
			if(!oldPath.empty())
				releasePlusDriveMirror(oldPath, this);
			m_plusDriveInterprocessLock.reset();
			m_plusDriveInterprocessLock = std::move(candidateLock);
		}
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			m_plusDriveAutoSavePath = targetPath;
			m_plusDrivePersistenceStatus = "Auto-save mirror is current: "
				+ _target.getFullPathName();
		}
		startPlusDrivePersistenceThread();
		m_plusDrivePersistenceCv.notify_all();
		_result = "+Drive auto-save mirror enabled at " + _target.getFullPathName();
		return true;
	}

	void AudioPluginAudioProcessor::disablePlusDriveAutoSave()
	{
		stopPlusDrivePersistenceThread();
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			m_plusDriveAutoSavePath.clear();
			m_plusDriveFlushedRequest = m_plusDriveFlushRequest;
			if(m_plusDrivePersistenceStatus.startsWith(
				"Final auto-save mirror failed:"))
				m_plusDrivePersistenceStatus = "Auto-save mirror disabled; "
					+ m_plusDrivePersistenceStatus;
			else
				m_plusDrivePersistenceStatus =
					"Project-owned +Drive; auto-save mirror disabled";
		}
		releasePlusDriveMirror(this);
		m_plusDriveInterprocessLock.reset();
	}

	bool AudioPluginAudioProcessor::plusDriveAutoSaveEnabled() const
	{
		std::lock_guard lock(m_plusDrivePersistenceMutex);
		return !m_plusDriveAutoSavePath.empty();
	}

	juce::File AudioPluginAudioProcessor::getPlusDriveAutoSaveFile() const
	{
		std::lock_guard lock(m_plusDrivePersistenceMutex);
		return juce::File(juce::String::fromUTF8(m_plusDriveAutoSavePath.c_str()));
	}

	juce::String AudioPluginAudioProcessor::getPlusDrivePersistenceStatus() const
	{
		std::lock_guard lock(m_plusDrivePersistenceMutex);
		return m_plusDrivePersistenceStatus;
	}

	void AudioPluginAudioProcessor::startPlusDrivePersistenceThread()
	{
		std::lock_guard lock(m_plusDrivePersistenceMutex);
		if(m_plusDrivePersistenceThread.joinable())
			return;
		m_plusDrivePersistenceStop = false;
		m_plusDrivePersistenceThread = std::thread(
			[this] { plusDrivePersistenceLoop(); });
	}

	void AudioPluginAudioProcessor::stopPlusDrivePersistenceThread()
	{
		{
			std::lock_guard lock(m_plusDrivePersistenceMutex);
			m_plusDrivePersistenceStop = true;
		}
		m_plusDrivePersistenceCv.notify_all();
		if(m_plusDrivePersistenceThread.joinable())
			m_plusDrivePersistenceThread.join();
	}

	void AudioPluginAudioProcessor::plusDrivePersistenceLoop()
	{
		using Clock = std::chrono::steady_clock;
		md::Device* observedDevice = nullptr;
		uint64_t observedEpoch = 0;
		uint64_t observedGeneration = 0;
		auto flushAfter = Clock::now();
		while(true)
		{
			std::string targetPath;
			uint64_t flushRequest = 0;
			bool forced = false;
			bool stopping = false;
			{
				std::unique_lock lock(m_plusDrivePersistenceMutex);
				m_plusDrivePersistenceCv.wait_for(lock, std::chrono::milliseconds(250),
					[this] { return m_plusDrivePersistenceStop; });
				stopping = m_plusDrivePersistenceStop;
				targetPath = m_plusDriveAutoSavePath;
				flushRequest = m_plusDriveFlushRequest;
				forced = flushRequest != m_plusDriveFlushedRequest;
			}
			if(targetPath.empty())
			{
				if(stopping)
					return;
				continue;
			}

			// A disabled, replaced or destroyed mirror gets one final current image.
			// This bypasses the debounce but retains the same atomic write and
			// generation-aware acknowledgement as a normal background flush.
			if(stopping)
			{
				std::unique_lock operationLock(m_storageLoadMutex);
				md::Device* deviceIdentity = nullptr;
				uint64_t epoch = 0;
				uint64_t generation = 0;
				std::vector<uint8_t> data;
				const bool captured = getPlugin().withDeviceLocked(
					[&](synthLib::Device* const _device)
					{
						auto* const device = dynamic_cast<md::Device*>(_device);
						if(!device
							|| device->getModel() != md::MachineModel::Machinedrum)
							return false;
						deviceIdentity = device;
						epoch = device->hardwareEpoch();
						generation = device->getHardware().plusDriveGeneration();
						data = device->getHardware().copyPlusDriveData();
						return true;
					});
				juce::String error;
				const juce::File target(
					juce::String::fromUTF8(targetPath.c_str()));
				const bool written = captured
					&& storageImage::writePlusDriveAtomically(target, data, error);
				{
					std::lock_guard lock(m_plusDrivePersistenceMutex);
					m_plusDrivePersistenceStatus = written
						? "Auto-save mirror is current: " + target.getFullPathName()
						: "Final auto-save mirror failed: "
							+ (captured ? error : "local Machinedrum unavailable");
					if(written && m_plusDriveAutoSavePath == targetPath)
						m_plusDriveFlushedRequest = flushRequest;
				}
				if(written)
					getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
					{
						auto* const device = dynamic_cast<md::Device*>(_device);
						if(device == deviceIdentity && device
							&& device->hardwareEpoch() == epoch)
							device->getHardware().markPlusDrivePersisted(generation);
					});
				return;
			}

			md::Device* deviceIdentity = nullptr;
			uint64_t epoch = 0;
			uint64_t generation = 0;
			bool dirty = false;
			getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				if(!device || device->getModel() != md::MachineModel::Machinedrum)
					return;
				deviceIdentity = device;
				epoch = device->hardwareEpoch();
				generation = device->getHardware().plusDriveGeneration();
				dirty = device->getHardware().plusDriveDirty();
			});
			if(!deviceIdentity || (!dirty && !forced))
				continue;

			const auto now = Clock::now();
			if(deviceIdentity != observedDevice || epoch != observedEpoch
				|| generation != observedGeneration)
			{
				observedDevice = deviceIdentity;
				observedEpoch = epoch;
				observedGeneration = generation;
				flushAfter = now + std::chrono::seconds(1);
				continue;
			}
			if(now < flushAfter)
				continue;
			std::unique_lock operationLock(m_storageLoadMutex, std::try_to_lock);
			if(!operationLock.owns_lock())
				continue;

			std::vector<uint8_t> data;
			const bool snapshotValid = getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					if(device != deviceIdentity || !device
						|| device->hardwareEpoch() != epoch
						|| device->getHardware().plusDriveGeneration() != generation)
						return false;
					data = device->getHardware().copyPlusDriveData();
					return true;
				});
			if(!snapshotValid)
				continue;

			juce::String error;
			const juce::File target(juce::String::fromUTF8(targetPath.c_str()));
			const bool written = storageImage::writePlusDriveAtomically(target, data, error);
			bool sameTarget = false;
			{
				std::lock_guard lock(m_plusDrivePersistenceMutex);
				sameTarget = m_plusDriveAutoSavePath == targetPath;
				m_plusDrivePersistenceStatus = written
					? "Auto-save mirror is current: " + target.getFullPathName()
					: "Auto-save mirror failed: " + error;
				if(written && sameTarget
					&& flushRequest > m_plusDriveFlushedRequest)
					m_plusDriveFlushedRequest = flushRequest;
			}
			if(written && sameTarget)
			{
				getPlugin().withDeviceLocked([&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					if(device == deviceIdentity && device
						&& device->hardwareEpoch() == epoch)
						device->getHardware().markPlusDrivePersisted(generation);
				});
			}
			flushAfter = Clock::now() + (written
				? std::chrono::seconds(1) : std::chrono::seconds(2));
		}
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
		EphemeralConfig, const bool _allowMcpServer) :
		AudioPluginAudioProcessor(_model, std::vector<uint8_t>{}, _allowMcpServer, true)
	{
	}

	AudioPluginAudioProcessor::AudioPluginAudioProcessor(const md::MachineModel _model,
		std::vector<uint8_t> _initialPatchRam, const bool _allowMcpServer,
		const bool _ephemeralConfig) :
		Processor(makeBuses(_model),
			getOptions(_model, _ephemeralConfig), makeProcessorProperties(_model),
			_allowMcpServer, _ephemeralConfig
				? jucePluginEditorLib::Processor::ConfigMode::Ephemeral
				: jucePluginEditorLib::Processor::ConfigMode::Persistent)
		, m_model(_model)
		, m_initialPatchRam(std::move(_initialPatchRam))
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
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		destroyEditorState();
		stopPlusDrivePersistenceThread();
		releasePlusDriveMirror(this);
		m_plusDriveInterprocessLock.reset();
	}

	jucePluginEditorLib::PluginEditorState* AudioPluginAudioProcessor::createEditorState()
	{
		return new PluginEditorState(*this);
	}

	synthLib::Device* AudioPluginAudioProcessor::createDevice()
	{
		synthLib::DeviceCreateParams params;
		params.customData = md::deviceCustomData(m_model);
		params.homePath = getDataFolder();
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
