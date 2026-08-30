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

	// A pristine MAME-compatible dump contains the compressed factory bank and
	// blank UW sample sectors. Run the firmware's one-time initializer without
	// real-time DSP background slices, then verify the persisted result on a
	// normal, freshly booted machine.
	advance(initializer, md::g_samplerate * 5);
	for(uint32_t instruction = 0; instruction < 200'000'000; ++instruction)
		initializer.processUC();
	if(!initializer.flashDirty())
		return fail("firmware did not initialize UW flash");

	const auto initializedFlash = initializer.copyFlashData();
	md::Hardware hardware(rom, argv[1], md::MachineModel::Machinedrum,
		{}, {}, {}, initializedFlash);
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

	std::cout << "Machinedrum UW ROM/RAM firmware test passed; ROM peak="
		<< peak << '\n';
	return 0;
}
