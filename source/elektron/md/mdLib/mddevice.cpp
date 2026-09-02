#include "mddevice.h"

#include "mdstate.h"
#include "mdromloader.h"
#include "mdtypes.h"

#include "baseLib/filesystem.h"

#include <cstdio>
namespace
{
	std::vector<uint8_t> loadInitialPatchRam(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model, const std::vector<uint8_t>& _initialPatchRam)
	{
		if(!_initialPatchRam.empty())
			return _initialPatchRam;
		if(_model != md::MachineModel::Monomachine || _params.homePath.empty())
			return {};

		const auto filename = baseLib::filesystem::validatePath(_params.homePath)
			+ "nvram/mm-factory-live3-be.bin";
		std::vector<uint8_t> data;
		if(!baseLib::filesystem::readFile(data, filename))
			return {};

		if(data.size() != md::g_patchRamStateSize)
		{
			std::fprintf(stderr,
				"[MM] ignoring factory patch RAM with unexpected size: %s (%zu bytes, expected %u)\n",
				filename.c_str(), data.size(), md::g_patchRamStateSize);
			return {};
		}

		std::fprintf(stderr, "[MM] factory patch RAM discovered at %s (%zu bytes)\n",
			filename.c_str(), data.size());
		return data;
	}

	std::string mdFlashCacheFilename(const std::string& _homePath,
		const md::MachineModel _model)
	{
		if(_model != md::MachineModel::Machinedrum || _homePath.empty())
			return {};
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/md-uw-1.63-factory-v2.cache";
	}

	std::string mdFlashCacheFilename(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model)
	{
		return mdFlashCacheFilename(_params.homePath, _model);
	}

	struct InitialMdFlash
	{
		std::vector<uint8_t> flash;
		std::vector<uint8_t> cache;
	};

	InitialMdFlash loadInitialMdFlash(
		const synthLib::DeviceCreateParams& _params, const md::MachineModel _model)
	{
		const auto filename = mdFlashCacheFilename(_params, _model);
		std::vector<uint8_t> cache;
		if(filename.empty() || !baseLib::filesystem::readFile(cache, filename))
			return {};

		md::Rom rom;
		if(!_params.romData.empty())
		{
			md::Rom supplied(_params.romData, _params.romName);
			if(supplied.isValid() && md::RomLoader::isRomForModel(
				supplied.data(), _model))
				rom = std::move(supplied);
		}
		if(!rom.isValid())
			rom = md::RomLoader::findROM(_model);

		InitialMdFlash result;
		if(!rom.isValid()
			|| !md::decodeFactoryFlashCache(result.flash, cache, rom.data()))
		{
			std::fprintf(stderr,
				"[MD] ignoring invalid or ROM-mismatched UW factory cache: %s\n",
				filename.c_str());
			return {};
		}
		result.cache = std::move(cache);
		return result;
	}

	InitialMdFlash loadInitialMdFlash(const md::Rom& _rom,
		const std::string& _filename)
	{
		InitialMdFlash result;
		if(!_rom.isValid() || _filename.empty()
			|| !baseLib::filesystem::readFile(result.cache, _filename)
			|| !md::decodeFactoryFlashCache(result.flash, result.cache, _rom.data()))
			return {};
		return result;
	}

	md::Rom loadStateRom(const std::vector<uint8_t>& _romData,
		const std::string& _romName,
		const md::MachineModel _model)
	{
		if(!_romData.empty())
		{
			md::Rom rom(_romData, _romName);
			if(rom.isValid() && md::RomLoader::isRomForModel(rom.data(), _model))
				return rom;
		}
		return md::RomLoader::findROM(_model);
	}
}

namespace md
{
	Device::Device(const synthLib::DeviceCreateParams& _params,
		const std::vector<uint8_t>& _initialPatchRam)
		: synthLib::Device(_params)
		, m_model(machineModelFromDeviceCustomData(_params.customData))
		, m_frontPanelPublisher(std::make_shared<FrontPanelPublisher>())
		, m_preparationContext(new PreparationContext(_params, m_model,
			m_frontPanelPublisher))
		, m_mdFlashCacheFilename(mdFlashCacheFilename(_params, m_model))
	{
		auto initialFlash = loadInitialMdFlash(_params, m_model);
		m_hardware = std::make_unique<Hardware>(_params.romData, _params.romName, m_model,
			loadInitialPatchRam(_params, m_model, _initialPatchRam), m_frontPanelPublisher,
			initialFlash.flash, initialFlash.cache);
	}

	bool Device::captureFactoryFlashCachePersistence(std::string& _filename,
		FactoryFlashSnapshot& _snapshot, std::string& _error)
	{
		_error.clear();
		_filename.clear();
		_snapshot = {};
		if(m_mdFlashCacheFilename.empty() || !m_hardware)
		{
			_error = "factory cache has no writable destination";
			return false;
		}
		if(!m_hardware->factoryFlashCacheReady())
		{
			_error = "factory cache is not ready";
			return false;
		}
		if(!m_hardware->copyFactoryFlashSnapshot(_snapshot))
		{
			_error = "validated factory cache could not be captured";
			return false;
		}
		_filename = m_mdFlashCacheFilename;
		return true;
	}

	bool Device::materializeFactoryFlashCache(FactoryFlashSnapshot& _snapshot,
		const std::shared_ptr<const PreparationContext>& _context,
		std::string& _error)
	{
		_error.clear();
		if(!_snapshot.cache.empty() || _snapshot.baseline.empty())
			return true;
		if(!_context)
		{
			_error = "factory cache has no preparation context";
			return false;
		}
		auto rom = loadStateRom(_context->m_romData, _context->m_romName,
			_context->m_model);
		if(!rom.isValid() || !encodeFactoryFlashCache(_snapshot.cache,
			_snapshot.baseline, rom.data()))
		{
			_error = "validated factory cache could not be encoded";
			return false;
		}
		_snapshot.baseline.clear();
		return true;
	}

	bool Device::writeFactoryFlashCachePersistence(const std::string& _filename,
		const std::vector<uint8_t>& _cache, std::string& _error)
	{
		_error.clear();
		if(_filename.empty() || _cache.empty())
		{
			_error = "factory cache persistence data is incomplete";
			return false;
		}

		// Project activity can never update a valid cache. Invalid or ROM-mismatched
		// data is replaced atomically so one damaged cache cannot poison every boot.
		// This method performs only filesystem work and is called outside the device
		// lock by the processor's first-run service.
		std::vector<uint8_t> existing;
		const bool exists = baseLib::filesystem::readFile(existing, _filename);
		if(exists && existing == _cache)
			return true;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(_filename));
		const bool written = exists
			? baseLib::filesystem::writeFileAtomic(_filename, _cache)
			: baseLib::filesystem::writeFileExclusive(_filename, _cache);
		if(written)
		{
			std::fprintf(stderr, "[MD] stored UW factory cache: %s\n",
				_filename.c_str());
			return true;
		}
		// A concurrent first-run instance may have won the exclusive create.
		existing.clear();
		if(baseLib::filesystem::readFile(existing, _filename) && existing == _cache)
			return true;
		_error = "could not store UW factory cache at " + _filename;
		return false;
	}

	float Device::getSamplerate() const
	{
		return g_samplerate;
	}

	bool Device::isValid() const
	{
		return m_hardware->isValid();
	}

	bool Device::getState(std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		if(isProjectStateRestorePending() && _type == m_requestedStateType
			&& m_requestedState)
		{
			_state.insert(_state.end(), m_requestedState->begin(), m_requestedState->end());
			return true;
		}

		auto* stateHardware = m_hardware.get();
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware)
			stateHardware = m_deferredPreparedState->m_hardware.get();
		const auto patchRam = stateHardware->copyPatchRam();
		if(m_model == MachineModel::Monomachine)
			return encodeState(_state, patchRam, m_model, _type);
		std::vector<uint8_t> factoryBaseline;
		if(stateHardware->copyFactoryFlashBaseline(factoryBaseline))
			return encodeStateWithFactoryBaseline(_state, patchRam,
				stateHardware->copyFlashData(),
				factoryBaseline, stateHardware->flashBaseline(), m_model, _type);
		FlashSectorOverlay pending;
		if(stateHardware->copyPendingFlashOverlay(pending))
			return encodeState(_state, patchRam, pending,
				stateHardware->flashBaseline(), m_model, _type);
		// If interaction happened before the first machine-local baseline was
		// captured, preserve a complete flash image. An absolute sector set records
		// ROM-equal deletions and lets the replacement boot coherently without waiting
		// for another factory-initialization pass.
		return encodeState(_state, patchRam, stateHardware->copyFlashData(),
			stateHardware->flashBaseline(), stateHardware->flashBaseline(), m_model, _type);
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		auto transaction = beginStateTransaction(
			std::make_shared<const std::vector<uint8_t>>(_state), _type);
		if(!transaction)
			return false;
		const auto preparationSucceeded = transaction->prepare();
		return finishStateTransaction(*transaction) && preparationSucceeded;
	}

	bool Device::StateTransactionImpl::prepare()
	{
		// Release an interrupted candidate before constructing the replacement. Both
		// operations can tear down or create a complete emulated machine.
		m_displaced.reset();
		m_prepared = m_state
			? Device::prepareState(m_context, *m_state, m_type,
				m_factoryFlash) : nullptr;
		return m_prepared != nullptr;
	}

	std::unique_ptr<synthLib::Device::StateTransaction> Device::beginStateTransaction(
		std::shared_ptr<const std::vector<uint8_t>> _state,
		const synthLib::StateType _type)
	{
		if(!_state)
			return {};
		FactoryFlashSnapshot factoryFlash;
		if(m_model == MachineModel::Machinedrum)
			(void)m_hardware->copyFactoryFlashSnapshot(factoryFlash);
		auto displaced = std::move(m_deferredPreparedState);
		++m_deferredStateGeneration;
		m_requestedState = _state;
		m_requestedStateType = _type;
		m_restoreStatus = ProjectStateRestoreStatus::Preparing;
		m_restoreError.clear();
		return std::unique_ptr<synthLib::Device::StateTransaction>(
			new StateTransactionImpl(m_preparationContext, std::move(_state), _type,
				std::move(factoryFlash), m_deferredStateGeneration,
				std::move(displaced)));
	}

	bool Device::finishStateTransaction(synthLib::Device::StateTransaction& _transaction)
	{
		auto* const transaction = dynamic_cast<StateTransactionImpl*>(&_transaction);
		if(!transaction || transaction->m_context != m_preparationContext
			|| transaction->m_generation != m_deferredStateGeneration)
			return false;
		if(!transaction->m_prepared)
		{
			failProjectStateRestore("The project state could not be prepared.");
			return false;
		}
		if(transaction->m_prepared->m_hardware->isProjectStateRestorePending())
		{
			m_deferredPreparedState = std::move(transaction->m_prepared);
			m_restoreStatus = ProjectStateRestoreStatus::Initializing;
			return true;
		}
		if(!commitPreparedState(*transaction->m_prepared))
		{
			failProjectStateRestore("The prepared project state could not be committed.");
			return false;
		}
		clearProjectStateRestore();
		return true;
	}

	std::unique_ptr<Device::PreparedState> Device::takeFinishedDeferredState(
		uint64_t& _generation)
	{
		if(!m_deferredPreparedState || !m_deferredPreparedState->m_hardware
			|| m_restoreStatus != ProjectStateRestoreStatus::Initializing
			|| m_deferredPreparedState->m_hardware->isProjectStateRestorePending())
			return {};
		_generation = m_deferredStateGeneration;
		m_restoreStatus = ProjectStateRestoreStatus::Finalizing;
		return std::move(m_deferredPreparedState);
	}

	std::unique_ptr<Device::PreparedState> Device::makeDeferredStateReboot(
		const PreparedState& _validated)
	{
		if(!_validated.m_context || !_validated.m_hardware
			|| !_validated.m_hardware->isValid()
			|| _validated.m_hardware->isProjectStateRestorePending())
			return {};
		const auto cache = _validated.m_hardware->copyFactoryFlashCache();
		if(cache.empty())
			return {};
		auto replacement = std::make_unique<Hardware>(
			_validated.m_context->m_romData, _validated.m_context->m_romName,
			_validated.m_context->m_model, _validated.m_hardware->copyPatchRam(),
			_validated.m_context->m_frontPanelPublisher,
			_validated.m_hardware->copyFlashData(), cache);
		if(!replacement->isValid())
			return {};
		return std::unique_ptr<PreparedState>(new PreparedState(
			_validated.m_context, std::move(replacement), true));
	}

	std::unique_ptr<Device::PreparedState> Device::prepareState(
		std::shared_ptr<const PreparationContext> _context,
		const std::vector<uint8_t>& _state, const synthLib::StateType _type,
		const FactoryFlashSnapshot& _factoryFlash)
	{
		if(!_context)
			return {};

		std::vector<uint8_t> patchRam;
		std::vector<uint8_t> initialFlash;
		bool containsFlash = false;
		if(_context->m_model == MachineModel::Monomachine)
		{
			if(!decodeState(patchRam, _state, _context->m_model, _type))
				return {};
		}
		else
		{
			auto stateRom = loadStateRom(_context->m_romData, _context->m_romName,
				_context->m_model);
			if(!stateRom.isValid())
				return {};
			DecodedState decoded;
			if(!decodeState(decoded, _state, stateRom.data(), _context->m_model, _type))
				return {};
			patchRam = std::move(decoded.patchRam);
			containsFlash = decoded.containsFlash;
			auto factory = loadInitialMdFlash(stateRom,
				mdFlashCacheFilename(_context->m_homePath, _context->m_model));
			if(factory.cache.empty() && !_factoryFlash.cache.empty())
			{
				if(!decodeFactoryFlashCache(factory.flash, _factoryFlash.cache,
					stateRom.data()))
					return {};
				factory.cache = _factoryFlash.cache;
			}
			else if(factory.cache.empty() && !_factoryFlash.baseline.empty())
			{
				factory.flash = _factoryFlash.baseline;
				if(!encodeFactoryFlashCache(factory.cache, factory.flash,
					stateRom.data()))
					return {};
			}
			FlashSectorOverlay pending;
			if(containsFlash)
			{
				if(!factory.flash.empty())
				{
					if(!applyFlashOverlay(initialFlash, decoded.flashOverlay,
						factory.flash, stateRom.data()))
						return {};
				}
				else if(decoded.flashOverlay.sectors.size()
					== g_romSize / g_uwFlashSectorSize)
				{
					// A complete overlay is baseline-independent. Materialize it before
					// the replacement starts so firmware boots from one coherent project.
					if(!applyFlashOverlay(initialFlash, decoded.flashOverlay,
						stateRom.data(), stateRom.data()))
						return {};
				}
				else
					pending = std::move(decoded.flashOverlay);
			}
			else if(!factory.flash.empty())
				initialFlash = factory.flash;

			auto replacement = std::make_unique<Hardware>(
				_context->m_romData, _context->m_romName, _context->m_model, patchRam,
				pending.valid ? std::shared_ptr<FrontPanelPublisher>{}
					: _context->m_frontPanelPublisher,
				initialFlash, factory.cache, pending);
			if(!replacement->isValid())
				return {};
			return std::unique_ptr<PreparedState>(
				new PreparedState(std::move(_context), std::move(replacement),
					containsFlash));
		}

		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName, _context->m_model, patchRam,
			_context->m_frontPanelPublisher, initialFlash);
		if(!replacement->isValid())
			return {};
		return std::unique_ptr<PreparedState>(
			new PreparedState(std::move(_context), std::move(replacement),
				containsFlash));
	}

	bool Device::commitPreparedState(PreparedState& _prepared)
	{
		if(_prepared.m_committed || _prepared.m_context != m_preparationContext
			|| !_prepared.m_hardware
			|| !_prepared.m_hardware->isValid())
			return false;

		const auto clockPercent = getDspClockPercent();
		_prepared.m_hardware->getDspMixer().getPeriph().getEssiClock()
			.setSpeedPercent(clockPercent);
		if(m_model == MachineModel::Machinedrum && !_prepared.m_containsFlash)
		{
			// Patch-only and legacy states preserve the current sample flash. Both
			// machines are stopped under the outer Device lock, so exchange ownership
			// of the multi-megabyte backing stores and factory-capture progress in O(1).
			if(!_prepared.m_hardware->exchangePersistentFlashState(*m_hardware))
				return false;
		}

		m_frontPanelPublisher->reset();
		m_hardware.swap(_prepared.m_hardware);
		++m_hardwareEpoch;
		_prepared.m_committed = true;
		m_numSamplesProcessed = 0;
		return true;
	}

	bool Device::commitDeferredStateRestore(PreparedState& _prepared,
		const uint64_t _generation)
	{
		if(_generation != m_deferredStateGeneration
			|| m_restoreStatus != ProjectStateRestoreStatus::Finalizing)
			return false;
		if(!commitPreparedState(_prepared))
		{
			failProjectStateRestore("The validated project state could not be committed.");
			return false;
		}
		clearProjectStateRestore();
		return true;
	}

	bool Device::rejectDeferredStateRestore(const uint64_t _generation,
		std::string _error)
	{
		if(_generation != m_deferredStateGeneration
			|| m_restoreStatus != ProjectStateRestoreStatus::Finalizing)
			return false;
		failProjectStateRestore(std::move(_error));
		return true;
	}

	void Device::clearProjectStateRestore()
	{
		m_deferredPreparedState.reset();
		m_requestedState.reset();
		m_restoreStatus = ProjectStateRestoreStatus::Idle;
		m_restoreError.clear();
	}

	void Device::failProjectStateRestore(std::string _error)
	{
		m_deferredPreparedState.reset();
		m_requestedState.reset();
		m_restoreStatus = ProjectStateRestoreStatus::Failed;
		m_restoreError = std::move(_error);
	}

	uint32_t Device::getChannelCountIn()
	{
		return m_model == MachineModel::Machinedrum ? 2 : 0;
	}

	uint32_t Device::getChannelCountOut()
	{
		return 2;
	}

	bool Device::setDspClockPercent(const uint32_t _percent)
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().setSpeedPercent(_percent);
	}

	uint32_t Device::getDspClockPercent() const
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().getSpeedPercent();
	}

	uint64_t Device::getDspClockHz() const
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().getSpeedInHz();
	}

	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		m_hardware->readMidiOut(_midiOut);
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, const size_t _samples)
	{
		if(m_model == MachineModel::Machinedrum)
			m_hardware->processAudio(_inputs, _outputs,
				static_cast<uint32_t>(_samples), getExtraLatencySamples());
		else
			m_hardware->processAudio(_outputs, static_cast<uint32_t>(_samples),
				getExtraLatencySamples());
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware
			&& m_deferredPreparedState->m_hardware->isProjectStateRestorePending())
			m_deferredPreparedState->m_hardware->advance(
				static_cast<uint32_t>(_samples));
		m_numSamplesProcessed += static_cast<uint32_t>(_samples);
	}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response)
	{
		if(_ev.sysex.empty())
		{
			const auto status = static_cast<uint8_t>(_ev.a & 0xf0);

			// The standalone MD plug-in has no valid host preset map yet, so retain its
			// historical Program Change suppression by default. An embedded host such as
			// MachineRack may opt in when it deliberately treats Program Change as a native
			// firmware performance message rather than a JUCE preset selection.
			if(m_model != MachineModel::Monomachine
				&& status == synthLib::M_PROGRAMCHANGE
				&& !m_nativeProgramChangesEnabled)
				return true;

			// Map MIDI note-on 36..51 onto the 16 front-panel TRIG keys (pads 1..16) - the PROVEN
			// MD audio-trigger path. The MM is a chromatic synth and instead receives every note,
			// velocity, and release through its native UART1 parser.
			constexpr uint8_t g_padNoteFirst = 36;	// pad 1 (BD)
			constexpr uint8_t g_padNoteLast  = 51;	// pad 16 (M4)
			const auto note = static_cast<uint8_t>(_ev.b);

			if(m_model != MachineModel::Monomachine &&
				(status == synthLib::M_NOTEON || status == synthLib::M_NOTEOFF) &&
				note >= g_padNoteFirst && note <= g_padNoteLast)
			{
				if(status == synthLib::M_NOTEON && _ev.c != 0)
				{
					const auto pad = static_cast<uint8_t>(note - g_padNoteFirst);	// 0..15
					const auto row = static_cast<uint8_t>(0x20 + (pad >> 3));		// 0x20: pads 1-8, 0x21: pads 9-16
					const auto bit = static_cast<uint8_t>(1u << (pad & 7));
					m_hardware->sendPanelEvent(row, bit);	// trig key press
					m_hardware->sendPanelEvent(row, 0x00);	// release
				}
				return true;
			}
		}

		auto e = _ev;
		e.offset += m_numSamplesProcessed + getExtraLatencySamples();
		m_hardware->sendMidi(e);	// other channel messages / sysex -> firmware UART1 RX on the MCU thread
		return true;
	}
}
