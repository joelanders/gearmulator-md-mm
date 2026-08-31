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

	std::string mdFlashCacheFilename(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model)
	{
		if(_model != md::MachineModel::Machinedrum || _params.homePath.empty())
			return {};
		return baseLib::filesystem::validatePath(_params.homePath)
			+ "nvram/md-uw-1.63-factory-v1.cache";
	}

	std::vector<uint8_t> loadInitialMdFlash(
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

		std::vector<uint8_t> flash;
		if(!rom.isValid() || !md::decodeFactoryFlashCache(flash, cache, rom.data()))
		{
			std::fprintf(stderr,
				"[MD] ignoring invalid or ROM-mismatched UW factory cache: %s\n",
				filename.c_str());
			return {};
		}
		return flash;
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
		, m_midiSysexProgressPublisher(
			std::make_shared<MidiSysexTransferProgressPublisher>())
		, m_preparationContext(new PreparationContext(_params, m_model,
			m_frontPanelPublisher, m_midiSysexProgressPublisher))
		, m_hardware(std::make_unique<Hardware>(_params.romData, _params.romName, m_model,
			loadInitialPatchRam(_params, m_model, _initialPatchRam), m_frontPanelPublisher,
			m_midiSysexProgressPublisher, loadInitialMdFlash(_params, m_model)))
		, m_mdFlashCacheFilename(mdFlashCacheFilename(_params, m_model))
	{
	}

	Device::~Device()
	{
		if(m_mdFlashCacheFilename.empty() || !m_hardware
			|| !m_hardware->factoryFlashCacheReady())
			return;

		// A valid cache is immutable: project activity can never update it. An
		// exclusive create also makes simultaneous first boots harmless.
		std::vector<uint8_t> existing;
		std::vector<uint8_t> decoded;
		if(baseLib::filesystem::readFile(existing, m_mdFlashCacheFilename)
			&& decodeFactoryFlashCache(decoded, existing, m_hardware->flashBaseline()))
			return;

		std::vector<uint8_t> cache;
		if(!encodeFactoryFlashCache(cache, m_hardware->copyFlashData(),
			m_hardware->flashBaseline()))
			return;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(m_mdFlashCacheFilename));
		if(baseLib::filesystem::writeFileExclusive(m_mdFlashCacheFilename, cache))
			std::fprintf(stderr, "[MD] created immutable UW factory cache: %s\n",
				m_mdFlashCacheFilename.c_str());
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
		const auto patchRam = m_hardware->copyPatchRam();
		if(m_model == MachineModel::Monomachine)
			return encodeState(_state, patchRam, m_model, _type,
				m_hardware->copyUserFlash());
		return encodeState(_state, patchRam, m_hardware->copyFlashData(),
			m_hardware->flashBaseline(), m_model, _type);
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		auto prepared = prepareState(m_preparationContext, _state, _type);
		return prepared && commitPreparedState(*prepared);
	}

	std::unique_ptr<Device::PreparedState> Device::prepareState(
		std::shared_ptr<const PreparationContext> _context,
		const std::vector<uint8_t>& _state, const synthLib::StateType _type)
	{
		if(!_context)
			return {};

		std::vector<uint8_t> patchRam;
		std::vector<uint8_t> initialFlash;
		bool containsFlash = false;
		if(_context->m_model == MachineModel::Monomachine)
		{
			if(!decodeState(patchRam, initialFlash, _state, _context->m_model, _type))
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
			if(containsFlash)
				initialFlash = std::move(decoded.flashData);
		}

		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName, _context->m_model, patchRam,
			_context->m_frontPanelPublisher, _context->m_midiSysexProgressPublisher,
			initialFlash);
		if(!replacement->isValid())
			return {};
		// A project restore is never evidence of a pristine factory initializer,
		// even if the firmware performs a later housekeeping flash write.
		if(_context->m_model == MachineModel::Machinedrum)
			replacement->disqualifyFactoryFlashCache();

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
			_prepared.m_hardware->replaceFlashData(m_hardware->copyFlashData(),
				m_hardware->flashDirty());

		m_frontPanelPublisher->reset();
		m_hardware.swap(_prepared.m_hardware);
		_prepared.m_committed = true;
		// Replacement aborts any transfer owned by the retired Hardware. Reset
		// only after the replacement has validated and become authoritative, so a
		// failed restore cannot erase a terminal observation from the live machine.
		m_midiSysexProgressPublisher->reset();
		m_numSamplesProcessed = 0;
		return true;
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
