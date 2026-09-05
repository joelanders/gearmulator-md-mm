#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdtypes.h"

#include "synthLib/plugin.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace md
{
	struct DevicePreparedStateTestAccess
	{
		static Hardware* deferredHardware(Device& _device)
		{
			return _device.m_deferredPreparedState
				? _device.m_deferredPreparedState->m_hardware.get() : nullptr;
		}

		static Hardware* preparedHardware(Device::PreparedState& _prepared)
		{
			return _prepared.m_hardware.get();
		}
	};
}

namespace
{
	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		constexpr uint32_t block = 128;
		for(uint32_t frames = 0; frames < _frames; frames += block)
			_hardware.advance(std::min(block, _frames - frames));
	}

	bool tap(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(md::MachineModel::Machinedrum, _control);
		if(!packet)
			return false;
		_hardware.sendPanelEvent(packet->row, packet->mask);
		advance(_hardware, 2048);
		_hardware.sendPanelEvent(packet->row, 0);
		advance(_hardware, 4096);
		return true;
	}

	bool sameLcd(const md::FrontPanel& _a, const md::FrontPanel& _b)
	{
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				if(_a.getLcdPixel(x, y) != _b.getLcdPixel(x, y))
					return false;
		return true;
	}

	bool initializeUwFlash(md::Hardware& _hardware,
		const std::function<void(md::Hardware&)>& _observe = {},
		const std::function<void(md::Hardware&)>& _beforeCapture = {})
	{
		advance(_hardware, md::g_samplerate * 5);
		if(_hardware.isFactoryFlashReadyForReboot())
			return true;
		for(uint32_t instruction = 0; instruction < 200'000'000; ++instruction)
			_hardware.processUC();
		for(uint32_t instruction = 0; instruction < 100'000'000; ++instruction)
		{
			_hardware.processUC();
			if((instruction & 1023u) == 0)
			{
				if(_beforeCapture)
					_beforeCapture(_hardware);
				// Zero-frame advances still run the bounded capture/publication tail,
				// exercising the same state machine used by an audio callback.
				_hardware.advance(0);
				if(_observe)
					_observe(_hardware);
				if(_hardware.isFactoryFlashReadyForReboot())
					return true;
			}
		}
		return _hardware.isFactoryFlashReadyForReboot();
	}

	void sendSysex(md::Hardware& _hardware,
		const std::initializer_list<uint8_t> _bytes)
	{
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
		event.sysex.assign(_bytes.begin(), _bytes.end());
		_hardware.sendMidi(event);
		advance(_hardware, 8192);
	}

	std::optional<uint8_t> queryLockMode(md::Hardware& _hardware)
	{
		// OS 1.63 Appendix C: status parameter 0x20 is the MD lock mode,
		// reported as 0 for Classic and 1 for Extended.
		std::vector<synthLib::SMidiEvent> discarded;
		_hardware.readMidiOut(discarded);
		synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
		request.sysex =
			{0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x70, 0x20, 0xf7};
		_hardware.sendMidi(request);

		for(uint32_t attempt = 0; attempt < 8; ++attempt)
		{
			advance(_hardware, 2048);
			std::vector<synthLib::SMidiEvent> events;
			_hardware.readMidiOut(events);
			for(const auto& event : events)
			{
				const auto& message = event.sysex;
				if(message.size() == 10
					&& message[0] == 0xf0 && message[1] == 0x00
					&& message[2] == 0x20 && message[3] == 0x3c
					&& message[4] == 0x02 && message[5] == 0x00
					&& message[6] == 0x72 && message[7] == 0x20
					&& message[8] <= 1 && message[9] == 0xf7)
					return message[8];
			}
		}
		return std::nullopt;
	}

	bool lockModeLedMatches(const md::FrontPanel& _panel, const uint8_t _mode)
	{
		const uint8_t raw = _panel.getLedBankRaw(md::FrontPanel::LedBank::Mode);
		return (raw & 0x03u) == (_mode == 0 ? 0x02u : 0x01u);
	}

	// Fit a periodic input with arbitrary phase and gain. Firmware latency and
	// the player envelope may change gain, but must preserve the waveform.
	double waveformCorrelation(const std::vector<float>& _audio, const size_t _begin,
		const size_t _count, const uint32_t _period)
	{
		double best = 0;
		for(uint32_t lag = 0; lag < _period; ++lag)
		{
			double x = 0, y = 0, xx = 0, yy = 0, xy = 0;
			for(size_t i = 0; i < _count; ++i)
			{
				const double reference = double((i + lag) % _period) / _period - 0.5;
				const double sample = _audio[_begin + i];
				x += reference; y += sample;
				xx += reference * reference; yy += sample * sample;
				xy += reference * sample;
			}
			const auto variance = (xx - x * x / _count) * (yy - y * y / _count);
			if(variance > 0)
				best = std::max(best, (xy - x * y / _count) / std::sqrt(variance));
		}
		return best;
	}

	bool recordedWaveformMatches(const std::array<std::vector<float>, 2>& _audio,
		const uint32_t _period, const uint32_t _otherPeriod, const bool _report = true)
	{
		// Find a sustained active region, then require both halves to match.
		// A single transient or a loud corrupted recording cannot pass.
		constexpr size_t window = 1024;
		for(const auto& channel : _audio)
		{
			if(!std::all_of(channel.begin(), channel.end(),
				[](const float sample) { return std::isfinite(sample); }))
				return false;
			double energy = 0, maximum = 0;
			size_t begin = 0;
			for(size_t i = 0; i < channel.size(); ++i)
			{
				energy += double(channel[i]) * channel[i];
				if(i >= window)
					energy -= double(channel[i - window]) * channel[i - window];
				if(i + 1 >= window && energy > maximum)
				{
					maximum = energy;
					begin = i + 1 - window;
				}
			}
			const auto rms = std::sqrt(maximum / window);
			if(rms < 0.003 || rms > 0.5)
				return false;
			for(size_t half = 0; half < 2; ++half)
			{
				const auto offset = begin + half * window / 2;
				const auto match = waveformCorrelation(channel, offset, window / 2, _period);
				const auto other = waveformCorrelation(channel, offset, window / 2, _otherPeriod);
				if(_report)
					std::cout << "RAM waveform period=" << _period << ", correlation="
						<< match << ", other input=" << other << ", RMS=" << rms << '\n';
				if(match < 0.90 || other > 0.35)
					return false;
			}
		}
		return true;
	}

	bool testAudioOracle()
	{
		std::array<std::vector<float>, 2> audio{
			std::vector<float>(2048), std::vector<float>(2048)};
		const auto matches = [&]() { return recordedWaveformMatches(audio, 97, 151, false); };
		if(matches()) // silence
			return false;
		for(auto& channel : audio)
			for(size_t i = 0; i < channel.size(); ++i)
				channel[i] = 0.2f * (float((i + 23) % 97) / 97.0f - 0.5f);
		if(!matches() || recordedWaveformMatches(audio, 151, 97, false))
			return false; // tolerate latency/gain, reject swapped inputs
		for(const auto invalid : {std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity()})
		{
			const auto previous = audio[1].back();
			audio[1].back() = invalid;
			if(matches())
				return false;
			audio[1].back() = previous;
		}
		for(auto& channel : audio)
		{
			std::fill(channel.begin(), channel.end(), 0);
			channel[500] = 0.2f;
		}
		if(matches()) // an audible transient is not a sustained recording
			return false;
		for(auto& channel : audio)
			for(size_t i = 0; i < channel.size(); ++i)
				channel[i] = i % 2 ? 0.02f : -0.02f;
		return !matches(); // structured noise resembling the old packing fault
	}

	void assignUwMachine(md::Hardware& _hardware, const uint8_t _track,
		const uint8_t _machine)
	{
		// Machinedrum OS 1.63 manual, Appendix C: assign machine. The final
		// argument selects the SPS-1UW machine table (ROM/RAM IDs 0..63).
		sendSysex(_hardware,
			{0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b,
				_track, _machine, 0x01, 0xf7});
	}

	void setTrack1Parameter(md::Hardware& _hardware, const uint8_t _parameter,
		const uint8_t _value)
	{
		_hardware.sendMidi({synthLib::MidiEventSource::Host,
			synthLib::M_CONTROLCHANGE, static_cast<uint8_t>(0x10 + _parameter),
			_value});
		advance(_hardware, 4096);
	}

	int fail(const char* const _message)
	{
		std::cerr << _message << '\n';
		return 1;
	}

	double benchmarkDevice(md::Device& _device, const uint32_t _blocks)
	{
		constexpr size_t blockSize = 128;
		std::array<std::vector<float>, 2> rendered{
			std::vector<float>(blockSize), std::vector<float>(blockSize)};
		synthLib::TAudioInputs inputs{};
		synthLib::TAudioOutputs outputs{};
		outputs[0] = rendered[0].data();
		outputs[1] = rendered[1].data();
		std::vector<synthLib::SMidiEvent> midiIn;
		std::vector<synthLib::SMidiEvent> midiOut;
		const auto begin = std::chrono::steady_clock::now();
		for(uint32_t block = 0; block < _blocks; ++block)
			_device.process(inputs, outputs, blockSize, midiIn, midiOut);
		const auto elapsed = std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - begin).count();
		return elapsed / static_cast<double>(_blocks);
	}
	constexpr uint32_t uwMemoryBegin = 0x180000;
	constexpr uint32_t uwMemoryEnd = 0x200000;
	constexpr uint32_t captureFrames = 16384;

	int testLockMode(md::Hardware& hardware)
	{
		// Tie the UI labels to firmware's own named lock-mode status rather than to
		// an assumed panel-bit order. This catches a Classic/Extended label swap
		// without having to construct and save two kits and patterns.
		const auto initialLockMode = queryLockMode(hardware);
		if(!initialLockMode
			|| !lockModeLedMatches(hardware.getFrontPanelSnapshot(), *initialLockMode))
			return fail("Machinedrum lock-mode LEDs disagree with firmware status");
		if(!tap(hardware, md::PanelControl::ClassicExtended))
			return fail("failed to toggle Machinedrum lock mode");
		const auto toggledLockMode = queryLockMode(hardware);
		if(!toggledLockMode || *toggledLockMode != (*initialLockMode ^ 1u)
			|| !lockModeLedMatches(hardware.getFrontPanelSnapshot(), *toggledLockMode))
			return fail("Machinedrum lock-mode toggle or LED mapping is wrong");
		if(!tap(hardware, md::PanelControl::ClassicExtended))
			return fail("failed to restore Machinedrum lock mode");
		const auto restoredLockMode = queryLockMode(hardware);
		if(!restoredLockMode || *restoredLockMode != *initialLockMode
			|| !lockModeLedMatches(hardware.getFrontPanelSnapshot(), *restoredLockMode))
			return fail("Machinedrum lock mode did not restore cleanly");
		return 0;
	}

	int testSilentRecording(md::Hardware& hardware, const md::PanelPacket& trigger,
		const md::PanelPacket& player)
	{
		// A non-default RAM-R RATE must preserve digital silence. Before MERGE was
		// implemented, the recorder packed each pair as $000800 instead of $800800;
		// RAM-P decoded that as a strong alternating tone.
		setTrack1Parameter(hardware, 7, 64);
		std::array<std::vector<float>, 2> silentCapture{
			std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
		synthLib::TAudioOutputs silentCaptureOutputs{};
		silentCaptureOutputs[0] = silentCapture[0].data();
		silentCaptureOutputs[1] = silentCapture[1].data();
		hardware.sendPanelEvent(trigger.row, trigger.mask);
		hardware.processAudio(silentCaptureOutputs, captureFrames, 0);
		hardware.sendPanelEvent(trigger.row, 0);
		advance(hardware, 4096);

		std::array<std::vector<float>, 2> silentPlayback{
			std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
		synthLib::TAudioOutputs silentPlaybackOutputs{};
		silentPlaybackOutputs[0] = silentPlayback[0].data();
		silentPlaybackOutputs[1] = silentPlayback[1].data();
		hardware.sendPanelEvent(player.row, player.mask);
		hardware.processAudio(silentPlaybackOutputs, captureFrames / 2, 0);
		hardware.sendPanelEvent(player.row, 0);
		silentPlaybackOutputs[0] += captureFrames / 2;
		silentPlaybackOutputs[1] += captureFrames / 2;
		hardware.processAudio(silentPlaybackOutputs, captureFrames / 2, 0);
		float silentRamPeak = 0.0f;
		for(const auto& channel : silentPlayback)
			for(size_t i = 0; i < channel.size(); ++i)
			{
				if(!std::isfinite(channel[i]))
					return fail("silent RAM playback produced a non-finite sample");
				if(i >= 4096)
					silentRamPeak = std::max(silentRamPeak, std::abs(channel[i]));
			}
		if(silentRamPeak > 0.0001f)
			return fail("RAM-R RATE introduced a tone into a silent recording");
		return 0;
	}

	int testExternalRecording(md::Hardware& hardware, const md::PanelPacket& trigger,
		const md::PanelPacket& player, const size_t inputSide)
	{
		setTrack1Parameter(hardware, 3, inputSide == 0 ? 0 : 127); // IBAL: A/B only
		setTrack1Parameter(hardware, 7, 127); // maximum-quality signal capture
		std::vector<dsp56k::TWord> uwMemoryBefore(uwMemoryEnd - uwMemoryBegin);
		auto& producerMemory = hardware.getDspProducer().dsp().memory();
		for(uint32_t address = uwMemoryBegin; address < uwMemoryEnd; ++address)
			uwMemoryBefore[address - uwMemoryBegin] =
				producerMemory.get(dsp56k::MemArea_X, address);

		std::array<std::vector<float>, 2> captureInput{
			std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
		for(uint32_t i = 0; i < captureFrames; ++i)
		{
			const auto phase = static_cast<float>(i % 97) / 97.0f;
			captureInput[0][i] = phase * 1.0f - 0.5f;
			captureInput[1][i] = static_cast<float>(i % 151) / 151.0f - 0.5f;
		}
		synthLib::TAudioInputs inputs{};
		inputs[0] = captureInput[0].data();
		inputs[1] = captureInput[1].data();
		std::array<std::vector<float>, 2> captureOutput{
			std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
		synthLib::TAudioOutputs recordingOutputs{};
		recordingOutputs[0] = captureOutput[0].data();
		recordingOutputs[1] = captureOutput[1].data();
		// The fixture performs long bare advances between host callbacks. Re-prime
		// the callback look-ahead before measuring synchronization; a real host keeps
		// this queue continuously fed.
		hardware.processAudio(recordingOutputs, 0, 1);
		hardware.processAudio(recordingOutputs, 0, 0);
		hardware.resetHostAudioInputQueueTelemetry();
		hardware.sendPanelEvent(trigger.row, trigger.mask);
		constexpr uint32_t hostBlockFrames = 256;
		for(uint32_t offset = 0; offset < captureFrames; offset += hostBlockFrames)
		{
			hardware.processAudio(inputs, recordingOutputs, hostBlockFrames, 0);
			for(auto& input : inputs)
				if(input)
					input += hostBlockFrames;
			for(auto& output : recordingOutputs)
				if(output)
					output += hostBlockFrames;
		}
		hardware.sendPanelEvent(trigger.row, 0);
		if(hardware.hostAudioInputUnderflowCount() != 0
			|| hardware.hostAudioInputOverflowCount() != 0)
		{
			for(size_t receiver = 0; receiver < 2; ++receiver)
				std::cerr << "DSP" << receiver + 1 << " codec input underflows="
					<< hardware.hostAudioInputUnderflowCount(receiver) << ", overflows="
					<< hardware.hostAudioInputOverflowCount(receiver) << '\n';
			return fail("RAM-R capture lost synchronization with the codec input bus");
		}
		advance(hardware, 4096);
		size_t changedUwWords = 0;
		for(uint32_t address = uwMemoryBegin; address < uwMemoryEnd; ++address)
			if(uwMemoryBefore[address - uwMemoryBegin]
				!= producerMemory.get(dsp56k::MemArea_X, address))
				++changedUwWords;
		if(changedUwWords < 100)
			return fail("RAM-R1 did not write a recording into UW sample memory");

		std::array<std::vector<float>, 2> ramRendered{
			std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
		synthLib::TAudioOutputs ramOutputs{};
		ramOutputs[0] = ramRendered[0].data();
		ramOutputs[1] = ramRendered[1].data();
		hardware.sendPanelEvent(player.row, player.mask);
		hardware.processAudio(ramOutputs, captureFrames / 2, 0);
		hardware.sendPanelEvent(player.row, 0);
		ramOutputs[0] += captureFrames / 2;
		ramOutputs[1] += captureFrames / 2;
		hardware.processAudio(ramOutputs, captureFrames / 2, 0);

		float ramPeak = 0.0f;
		for(const auto& channel : ramRendered)
			for(const auto sample : channel)
				ramPeak = std::max(ramPeak, std::abs(sample));
		if(ramPeak < 0.001f)
			return fail("RAM-P1 produced no audible recording from the external input");
		if(!recordedWaveformMatches(ramRendered, inputSide == 0 ? 97 : 151,
			inputSide == 0 ? 151 : 97))
			return fail("RAM-P1 did not preserve the selected external input waveform");
		std::cout << "RAM input " << inputSide << " peak=" << ramPeak << '\n';
		return 0;
	}

	int testEmptyMainMixLink(md::Hardware& hardware)
	{
		// DSP2 receives the main mix over ESSI0; external codec input uses ESSI1.
		// With no wire word pending, an RX tick must not reuse its retained RX word,
		// assert RDF, or trigger DMA: repeated retained words become pitched tones in
		// a recording. Rewire only after the audio checks because this deliberately
		// consumes the live test transport.
		auto& recorderLink = hardware.getDspProducer().getPeriph().getEssi0();
		if(!recorderLink.isFastLinkRx() || !recorderLink.hasEnabledReceivers())
			return fail("RAM-R DSP2 receive link was not active");
		auto& recorderInput = recorderLink.getAudioInputs();
		while(!recorderInput.empty())
			recorderInput.pop_front();
		const auto emptyLinkReads = std::make_shared<uint32_t>(0);
		recorderLink.setReadRxCallback(
			[emptyLinkReads](uint64_t& _frameIndex, dsp56k::Audio::RxFrame& _frame)
			{
				++*emptyLinkReads;
				_frame.clear();
				++_frameIndex;
			});
		recorderLink.execRX();
		if(*emptyLinkReads != 0)
			return fail("RAM-R DSP2 fabricated an RX edge on an empty serial link");
		return 0;
	}
}

static int runFirmwareTest(const char* const firmwarePath)
{
	std::ifstream input(firmwarePath, std::ios::binary);
	const std::vector<uint8_t> rom{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	synthLib::DeviceCreateParams deviceParams;
	deviceParams.romData = rom;
	deviceParams.romName = firmwarePath;
	deviceParams.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
	md::Device device(deviceParams);
	auto& initializer = device.getHardware();
	if(!initializer.isValid())
		return fail("firmware is not the supported Machinedrum OS 1.63 image");

	// An early panel/MIDI interaction must disqualify a reusable global cache without
	// stranding the firmware at its first-run reboot prompt. Preserve the complete
	// project image and cold-boot it through the same Device exchange used by the
	// processor's automatic recovery service.
	initializer.disqualifyFactoryFlashCache();
	const auto initialized = initializeUwFlash(initializer);
	if(!initializer.flashDirty())
		return fail("firmware did not initialize UW flash");
	if(!initialized)
		return fail("interacted UW initializer never became reboot-ready");
	if(!initializer.isFactoryFlashCaptureDisqualified()
		|| initializer.isFactoryFlashCacheReady())
		return fail("early interaction published a reusable UW factory cache");

	const auto initializedFlash = initializer.copyFlashData();
	std::vector<uint8_t> initializedState;
	if(!device.getState(initializedState, synthLib::StateTypeGlobal))
		return fail("could not capture interacted first-run UW state");
	auto firstRunReboot = md::Device::prepareState(device.getPreparationContext(),
		initializedState, synthLib::StateTypeGlobal);
	if(!firstRunReboot || !device.commitPreparedState(*firstRunReboot)
		|| device.getHardware().copyFlashData() != initializedFlash
		|| device.getHardware().isFactoryFlashInitializationExpected())
		return fail("interacted first-run UW state did not cold-boot coherently");
	firstRunReboot.reset();

	// The known-good initialized image supplies the fixture baseline for subsequent
	// cache and deferred-restore checks, even though the interacted boot correctly
	// refused to publish it as a machine-local cache.
	std::vector<uint8_t> factoryCache;
	if(!md::encodeFactoryFlashCache(factoryCache, initializedFlash, rom)
		|| factoryCache.size() >= rom.size())
		return fail("UW factory cache was not sparse");

	// A legacy/patch-only state must retain the exact live sample flash. The cold
	// reboot candidate starts without that image; commit transfers ownership of it
	// and the associated factory-capture policy instead of copying 8 MiB under the
	// process lock.
	auto legacyPatch = device.getHardware().copyPatchRam();
	legacyPatch[17] ^= 0x45;
	std::vector<uint8_t> legacyState;
	if(!md::encodeState(legacyState, legacyPatch,
		md::MachineModel::Machinedrum, synthLib::StateTypeGlobal))
		return fail("could not encode legacy patch-only state");
	auto patchOnly = md::Device::prepareState(device.getPreparationContext(),
		legacyState, synthLib::StateTypeGlobal);
	auto* const patchOnlyHardware = patchOnly
		? md::DevicePreparedStateTestAccess::preparedHardware(*patchOnly) : nullptr;
	if(!patchOnlyHardware
		|| !patchOnlyHardware->isFactoryFlashInitializationExpected()
		|| !device.commitPreparedState(*patchOnly)
		|| &device.getHardware() != patchOnlyHardware
		|| device.getHardware().copyFlashData() != initializedFlash
		|| device.getHardware().copyPatchRam() != legacyPatch
		|| device.getHardware().isFactoryFlashInitializationExpected())
		return fail("patch-only cold reboot did not transfer live flash state");
	patchOnly.reset();

	// A project may arrive before this machine has a local factory cache. Keep its
	// user sectors pending until initialization completes, then apply them without
	// allowing them into the factory cache.
	auto projectFlash = initializedFlash;
	projectFlash[6 * md::g_uwFlashSectorSize + 123] ^= 0x5a;
	projectFlash[10 * md::g_uwFlashSectorSize + 321] ^= 0x33;
	auto projectPatch = device.getHardware().copyPatchRam();
	projectPatch.front() ^= 0x6d;
	projectPatch.back() ^= 0x27;
	std::vector<uint8_t> projectState;
	if(!md::encodeStateWithFactoryBaseline(projectState, projectPatch, projectFlash,
		initializedFlash, rom, md::MachineModel::Machinedrum,
		synthLib::StateTypeGlobal))
		return fail("could not encode deferred UW project flash");
	md::DecodedState decodedProject;
	if(!md::decodeState(decodedProject, projectState, rom,
		md::MachineModel::Machinedrum, synthLib::StateTypeGlobal))
		return fail("could not decode deferred UW project flash");
	auto* const liveBeforeRestore = &device.getHardware();
	(void)benchmarkDevice(device, 64);
	const auto liveMicrosPerBlock = benchmarkDevice(device, 128);
	synthLib::Plugin plugin(&device, [](synthLib::Device* const _device)
	{
		return _device;
	});
	std::vector<uint8_t> hostProjectState{1, synthLib::StateTypeGlobal};
	hostProjectState.insert(hostProjectState.end(), projectState.begin(), projectState.end());
	if(!plugin.setState(hostProjectState)
		|| !device.hasDeferredStateRestore()
		|| &device.getHardware() != liveBeforeRestore)
		return fail("deferred UW state replaced the live machine before validation");
	std::vector<uint8_t> stateDuringInitialization;
	if(!plugin.getState(stateDuringInitialization, synthLib::StateTypeGlobal)
		|| stateDuringInitialization != hostProjectState)
		return fail("autosave did not preserve the requested state during initialization");
	(void)benchmarkDevice(device, 64);
	const auto restoringMicrosPerBlock = benchmarkDevice(device, 128);
	const auto restoreCostRatio = restoringMicrosPerBlock / liveMicrosPerBlock;
	std::cout << "UW restore callback benchmark: live " << liveMicrosPerBlock
		<< " us/block, live+candidate " << restoringMicrosPerBlock
		<< " us/block (" << restoreCostRatio << "x)\n";
	if(restoreCostRatio > 4.0)
		return fail("deferred restore callback cost exceeded the 4x safety bound");
	auto* const deferred = md::DevicePreparedStateTestAccess::deferredHardware(device);
	if(!deferred)
		return fail("deferred UW project state did not create an isolated candidate");
	md::FlashSectorOverlay pendingCheck;
	if(!deferred->copyPendingFlashOverlay(pendingCheck)
		|| pendingCheck.data != decodedProject.flashOverlay.data)
		return fail("deferred UW project flash was not queued");
	bool partialStateObserved = false;
	bool synchronousRestoreObserved = false;
	std::vector<uint8_t> preRestorePatch;
	constexpr uint64_t ucClockHz = 40'000'000;
	const auto deferredInitialized = initializeUwFlash(*deferred,
		[&](md::Hardware& _current)
		{
			const auto& uc = _current.getUC();
			if(uc.getCycles() < ucClockHz * 10
				|| uc.flashIdleCycles() < ucClockHz * 2)
				return;
			const auto flash = _current.copyFlashData();
			if(flash != initializedFlash && flash != projectFlash)
				partialStateObserved = true;
			const auto livePatch = uc.copyPatchRam();
			if(preRestorePatch.empty())
				preRestorePatch = livePatch;
			else if(livePatch != preRestorePatch && livePatch != projectPatch)
				partialStateObserved = true;
		},
		[&](md::Hardware& _current)
		{
			// Queries must not revive the old synchronous full-image restore path.
			if(_current.factoryFlashCacheReady())
				synchronousRestoreObserved = true;
		});
	const auto deferredFlash = deferred->copyFlashData();
	if(deferredFlash != projectFlash)
	{
		for(size_t i = 0; i < deferredFlash.size(); ++i)
			if(deferredFlash[i] != projectFlash[i])
			{
				std::cerr << "first deferred flash mismatch at " << i
					<< ": got " << static_cast<uint32_t>(deferredFlash[i])
					<< ", expected " << static_cast<uint32_t>(projectFlash[i]) << '\n';
				break;
			}
	}
	if(!deferredInitialized || !deferred->isValid() || partialStateObserved
		|| synchronousRestoreObserved
		|| deferredFlash != projectFlash
		|| deferred->getUC().copyPatchRam() != projectPatch
		|| &device.getHardware() != liveBeforeRestore)
		return fail("deferred UW project flash was not restored after initialization");
	uint64_t deferredGeneration = 0;
	auto validated = device.takeFinishedDeferredState(deferredGeneration);
	if(!validated || !device.hasDeferredStateRestore()
		|| device.projectStateRestoreStatus()
			!= md::Device::ProjectStateRestoreStatus::Finalizing)
		return fail("validated UW project state was not ready for a cold reboot");
	std::vector<uint8_t> stateDuringFinalization;
	if(!plugin.getState(stateDuringFinalization, synthLib::StateTypeGlobal)
		|| stateDuringFinalization != hostProjectState)
		return fail("autosave exposed the old machine during finalization");
	auto reboot = md::Device::makeDeferredStateReboot(*validated);
	if(!reboot)
		return fail("validated UW project state could not be cold-booted");
	auto* const rebootHardware =
		md::DevicePreparedStateTestAccess::preparedHardware(*reboot);
	if(!device.commitDeferredStateRestore(*reboot, deferredGeneration)
		|| &device.getHardware() != rebootHardware
		|| md::DevicePreparedStateTestAccess::preparedHardware(*reboot)
			!= liveBeforeRestore
		|| device.isProjectStateRestorePending())
		return fail("validated UW project state was not exchanged atomically");
	std::vector<uint8_t> deferredFactory;
	const auto deferredCache = device.getHardware().copyFactoryFlashCache();
	if(!md::decodeFactoryFlashCache(deferredFactory, deferredCache, rom)
		|| deferredFactory != initializedFlash)
		return fail("deferred project data contaminated the UW factory cache");
	if(device.getHardware().copyFlashData() != projectFlash
		|| device.getHardware().copyPatchRam() != projectPatch)
		return fail("cold reboot did not preserve the validated UW project images");

	// Once a healthy baseline is live, a state tied to any other factory image
	// must be rejected before it can replace that machine.
	auto wrongFactory = initializedFlash;
	wrongFactory[2 * md::g_uwFlashSectorSize + 17] ^= 0x41;
	auto wrongProjectFlash = wrongFactory;
	wrongProjectFlash[12 * md::g_uwFlashSectorSize + 91] ^= 0x24;
	std::vector<uint8_t> wrongFactoryState;
	if(!md::encodeStateWithFactoryBaseline(wrongFactoryState, projectPatch,
		wrongProjectFlash, wrongFactory, rom, md::MachineModel::Machinedrum,
		synthLib::StateTypeGlobal))
		return fail("could not encode wrong-factory UW project state");
	auto* const healthyHardware = &device.getHardware();
	if(device.setState(wrongFactoryState, synthLib::StateTypeGlobal)
		|| &device.getHardware() != healthyHardware
		|| device.getHardware().copyFlashData() != projectFlash
		|| device.projectStateRestoreStatus()
			!= md::Device::ProjectStateRestoreStatus::Failed
		|| device.projectStateRestoreError().empty())
		return fail("wrong-factory UW state replaced the healthy live machine");

	auto hardwareStorage = std::make_unique<md::Hardware>(rom, firmwarePath,
		md::MachineModel::Machinedrum, std::vector<uint8_t>{},
		std::shared_ptr<md::FrontPanelPublisher>{}, initializedFlash, factoryCache);
	auto& hardware = *hardwareStorage;
	advance(hardware, md::g_samplerate * 20);

	if(testLockMode(hardware))
		return 1;

	if(!tap(hardware, md::PanelControl::Kit)
		|| !tap(hardware, md::PanelControl::Down)
		|| !tap(hardware, md::PanelControl::Enter))
		return fail("failed to enter the machine picker");

	for(uint32_t family = 0; family < 6; ++family)
		if(!tap(hardware, md::PanelControl::Down))
			return fail("failed to navigate to CTR");
	const auto ctr = hardware.getFrontPanelSnapshot();

	tap(hardware, md::PanelControl::Down);
	const auto romFamily = hardware.getFrontPanelSnapshot();
	tap(hardware, md::PanelControl::Down);
	const auto ramFamily = hardware.getFrontPanelSnapshot();
	tap(hardware, md::PanelControl::Down);
	const auto afterRam = hardware.getFrontPanelSnapshot();
	if(sameLcd(ctr, romFamily) || sameLcd(romFamily, ramFamily)
		|| !sameLcd(ramFamily, afterRam))
		return fail("ROM/RAM machine families were not exposed in the expected order");

	// Return to ROM, select its current factory sample, assign it to track 1,
	// and prove that the resulting machine reaches the audio output.
	tap(hardware, md::PanelControl::Up);
	tap(hardware, md::PanelControl::Right);
	tap(hardware, md::PanelControl::Enter);
	tap(hardware, md::PanelControl::Exit);
	const auto trigger = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Trigger1);
	if(!trigger)
		return fail("trigger 1 has no panel mapping");

	std::array<std::vector<float>, 2> rendered{
		std::vector<float>(8192), std::vector<float>(8192)};
	synthLib::TAudioOutputs outputs{};
	outputs[0] = rendered[0].data();
	outputs[1] = rendered[1].data();
	hardware.sendPanelEvent(trigger->row, trigger->mask);
	hardware.processAudio(outputs, 4096, 0);
	hardware.sendPanelEvent(trigger->row, 0);
	outputs[0] += 4096;
	outputs[1] += 4096;
	hardware.processAudio(outputs, 4096, 0);

	float peak = 0.0f;
	for(const auto& channel : rendered)
		for(const auto sample : channel)
		{
			if(!std::isfinite(sample))
				return fail("factory ROM machine produced a non-finite sample");
			peak = std::max(peak, std::abs(sample));
		}
	if(peak < 0.001f)
		return fail("factory ROM machine produced no audible output");

	// Assign the paired RAM recorder/player from the OS 1.63 UW machine table.
	// RAM-R1 records the external inputs only, for one sequencer step, at full
	// rate. RAM-P1 then proves that the captured samples reached DSP memory.
	assignUwMachine(hardware, 0, 32);
	assignUwMachine(hardware, 1, 34);
	setTrack1Parameter(hardware, 0, 0);   // MLEV -64: no internal feedback
	setTrack1Parameter(hardware, 1, 64);  // MBAL centered
	setTrack1Parameter(hardware, 2, 64);  // ILEV 0: unity external input
	setTrack1Parameter(hardware, 3, 64);  // IBAL centered
	setTrack1Parameter(hardware, 4, 0);   // CUE1 off
	setTrack1Parameter(hardware, 5, 0);   // CUE2 off
	setTrack1Parameter(hardware, 6, 4);   // LEN: one sequencer step
	const auto player = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Trigger2);
	if(!player)
		return fail("trigger 2 has no panel mapping");

	if(testSilentRecording(hardware, *trigger, *player))
		return 1;

	if(testExternalRecording(hardware, *trigger, *player, 0)
		|| testExternalRecording(hardware, *trigger, *player, 1))
		return 1;

	if(testEmptyMainMixLink(hardware))
		return 1;

	std::cout << "Machinedrum UW ROM/RAM firmware test passed; ROM peak="
		<< peak << '\n';
	return 0;
}

int main(const int argc, const char* const* argv)
{
	if(argc == 2 && std::string(argv[1]) == "--audio-oracle")
		return testAudioOracle() ? 0 : fail("RAM audio oracle accepted corrupt audio or rejected its reference");
	if(argc > 2)
	{
		std::cerr << "usage: mdUwFirmwareTest [elektron_sps1-1uw_os1.63.bin]\n";
		return 2;
	}
	const char* const firmwarePath = argc == 2 ? argv[1]
		: std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	if(!firmwarePath || !*firmwarePath)
	{
		const auto* const required =
			std::getenv("GEARMULATOR_REQUIRE_FIRMWARE_TESTS");
		if(required && std::string(required) == "1")
		{
			std::cerr << "mdUwFirmwareTest: required MD firmware fixture is unavailable\n";
			return 1;
		}
		std::cout << "mdUwFirmwareTest: SKIP (pinned MD firmware not supplied)\n";
		return 77;
	}

	// Keep the firmware-heavy test in a separate stack frame. md::Hardware is
	// large enough that MSVC otherwise overflows the stack before this skip path
	// can run on fixture-free Windows CI hosts.
	return runFirmwareTest(firmwarePath);
}
