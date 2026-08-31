#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

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

	bool initializeUwFlash(md::Hardware& _hardware)
	{
		advance(_hardware, md::g_samplerate * 5);
		if(_hardware.factoryFlashCacheReady())
			return true;
		for(uint32_t instruction = 0; instruction < 200'000'000; ++instruction)
			_hardware.processUC();
		for(uint32_t instruction = 0; instruction < 100'000'000; ++instruction)
		{
			_hardware.processUC();
			if((instruction & 1023u) == 0 && _hardware.factoryFlashCacheReady())
				return true;
		}
		return _hardware.factoryFlashCacheReady();
	}

	void sendSysex(md::Hardware& _hardware,
		const std::initializer_list<uint8_t> _bytes)
	{
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
		event.sysex.assign(_bytes.begin(), _bytes.end());
		_hardware.sendMidi(event);
		advance(_hardware, 8192);
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
}

int main(const int argc, const char* const* argv)
{
	if(argc != 2)
	{
		std::cerr << "usage: mdUwFirmwareTest <elektron_sps1-1uw_os1.63.bin>\n";
		return 2;
	}

	std::ifstream input(argv[1], std::ios::binary);
	const std::vector<uint8_t> rom{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	md::Hardware initializer(rom, argv[1], md::MachineModel::Machinedrum);
	if(!initializer.isValid())
		return fail("firmware is not the supported Machinedrum OS 1.63 image");

	// Boot once to let the firmware prepare UW flash, then verify the resulting
	// factory baseline on a normal, freshly booted machine.
	const auto initialized = initializeUwFlash(initializer);
	if(!initializer.flashDirty())
		return fail("firmware did not initialize UW flash");
	if(!initialized)
		return fail("completed UW initializer was not eligible for the factory cache");

	const auto initializedFlash = initializer.copyFlashData();
	std::vector<uint8_t> factoryCache;
	if(!md::encodeFactoryFlashCache(factoryCache, initializedFlash, rom)
		|| factoryCache.size() >= rom.size())
		return fail("UW factory cache was not sparse");

	// A project may arrive before this machine has a local factory cache. Keep its
	// user sectors pending until initialization completes, then apply them without
	// allowing them into the factory cache.
	auto projectFlash = initializedFlash;
	projectFlash[6 * md::g_uwFlashSectorSize + 123] ^= 0x5a;
	std::vector<uint8_t> projectState;
	if(!md::encodeState(projectState, initializer.copyPatchRam(), projectFlash,
		initializedFlash, rom, md::MachineModel::Machinedrum,
		synthLib::StateTypeGlobal))
		return fail("could not encode deferred UW project flash");
	md::DecodedState decodedProject;
	if(!md::decodeState(decodedProject, projectState, rom,
		md::MachineModel::Machinedrum, synthLib::StateTypeGlobal))
		return fail("could not decode deferred UW project flash");
	md::Hardware deferred(rom, argv[1], md::MachineModel::Machinedrum,
		decodedProject.patchRam, {}, {}, {}, {}, decodedProject.flashOverlay);
	md::FlashSectorOverlay pendingCheck;
	if(!deferred.copyPendingFlashOverlay(pendingCheck)
		|| pendingCheck.data != decodedProject.flashOverlay.data)
		return fail("deferred UW project flash was not queued");
	const auto deferredInitialized = initializeUwFlash(deferred);
	const auto deferredFlash = deferred.copyFlashData();
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
	if(!deferredInitialized || !deferred.isValid() || deferredFlash != projectFlash)
		return fail("deferred UW project flash was not restored after initialization");
	std::vector<uint8_t> deferredFactory;
	const auto deferredCache = deferred.copyFactoryFlashCache();
	if(!md::decodeFactoryFlashCache(deferredFactory, deferredCache, rom)
		|| deferredFactory != initializedFlash)
		return fail("deferred project data contaminated the UW factory cache");

	md::Hardware hardware(rom, argv[1], md::MachineModel::Machinedrum,
		{}, {}, {}, initializedFlash, factoryCache);
	advance(hardware, md::g_samplerate * 20);

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
			peak = std::max(peak, std::abs(sample));
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
	setTrack1Parameter(hardware, 7, 127); // RATE: maximum quality
	constexpr uint32_t uwMemoryBegin = 0x180000;
	constexpr uint32_t uwMemoryEnd = 0x200000;
	std::vector<dsp56k::TWord> uwMemoryBefore(uwMemoryEnd - uwMemoryBegin);
	auto& producerMemory = hardware.getDspProducer().dsp().memory();
	for(uint32_t address = uwMemoryBegin; address < uwMemoryEnd; ++address)
		uwMemoryBefore[address - uwMemoryBegin] =
			producerMemory.get(dsp56k::MemArea_X, address);

	constexpr uint32_t captureFrames = 16384;
	std::array<std::vector<float>, 2> captureInput{
		std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
	for(uint32_t i = 0; i < captureFrames; ++i)
	{
		const auto phase = static_cast<float>(i % 97) / 97.0f;
		captureInput[0][i] = phase * 1.0f - 0.5f;
		captureInput[1][i] = captureInput[0][i];
	}
	synthLib::TAudioInputs inputs{};
	inputs[0] = captureInput[0].data();
	inputs[1] = captureInput[1].data();
	std::array<std::vector<float>, 2> captureOutput{
		std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
	synthLib::TAudioOutputs recordingOutputs{};
	recordingOutputs[0] = captureOutput[0].data();
	recordingOutputs[1] = captureOutput[1].data();
	hardware.sendPanelEvent(trigger->row, trigger->mask);
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
	hardware.sendPanelEvent(trigger->row, 0);
	advance(hardware, 4096);
	size_t changedUwWords = 0;
	for(uint32_t address = uwMemoryBegin; address < uwMemoryEnd; ++address)
		if(uwMemoryBefore[address - uwMemoryBegin]
			!= producerMemory.get(dsp56k::MemArea_X, address))
			++changedUwWords;
	if(changedUwWords < 100)
		return fail("RAM-R1 did not write a recording into UW sample memory");

	const auto player = md::panelPacket(md::MachineModel::Machinedrum,
		md::PanelControl::Trigger2);
	if(!player)
		return fail("trigger 2 has no panel mapping");
	std::array<std::vector<float>, 2> ramRendered{
		std::vector<float>(captureFrames), std::vector<float>(captureFrames)};
	synthLib::TAudioOutputs ramOutputs{};
	ramOutputs[0] = ramRendered[0].data();
	ramOutputs[1] = ramRendered[1].data();
	hardware.sendPanelEvent(player->row, player->mask);
	hardware.processAudio(ramOutputs, captureFrames / 2, 0);
	hardware.sendPanelEvent(player->row, 0);
	ramOutputs[0] += captureFrames / 2;
	ramOutputs[1] += captureFrames / 2;
	hardware.processAudio(ramOutputs, captureFrames / 2, 0);

	float ramPeak = 0.0f;
	for(const auto& channel : ramRendered)
		for(const auto sample : channel)
			ramPeak = std::max(ramPeak, std::abs(sample));
	if(ramPeak < 0.001f)
		return fail("RAM-P1 produced no audible recording from the external input");

	std::cout << "Machinedrum UW ROM/RAM firmware test passed; ROM peak="
		<< peak << ", RAM peak=" << ramPeak << '\n';
	return 0;
}
