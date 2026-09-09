#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
	void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}
	void advance(md::Hardware& hardware, uint32_t frames)
	{
		while(frames)
		{
			const auto n = std::min(frames, 256u);
			hardware.advance(n);
			frames -= n;
		}
	}
	bool locked(md::Hardware& hardware, unsigned encoder)
	{
		// Observable LCD lock inversion, not firmware memory. Both stock panel
		// layouts show the eight parameter cells as four columns and two rows.
		const auto panel = hardware.getFrontPanelSnapshot();
		const unsigned x = 51 + 20 * (encoder % 4), y = 13 + 31 * (encoder / 4);
		return panel.getLcdPixel(x, y) && panel.getLcdPixel(x + 12, y);
	}
}

int main(int argc, char** argv)
{
	if(argc < 2) return 1;
	const auto model = std::string_view(argv[1]) == "md"
		? md::MachineModel::Machinedrum : md::MachineModel::Monomachine;
	const char* path = std::getenv(model == md::MachineModel::Machinedrum
		? "GEARMULATOR_MD_FIRMWARE_BIN" : "GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path || !*path) { std::cout << "SKIP: firmware not supplied\n"; return 77; }
	const bool dropPress = argc > 2 && std::string_view(argv[2]) == "--drop-press";
	const bool dropRelease = argc > 2 && std::string_view(argv[2]) == "--drop-release";
	try
	{
		synthLib::DeviceCreateParams params;
		require(baseLib::filesystem::readFile(params.romData, path), "cannot read firmware");
		require(md::RomLoader::isRomForModel(params.romData, model), "wrong firmware model");
		params.romName = path;
		params.customData = md::deviceCustomData(model);
		// No homePath: never load or overwrite the user's machine storage.
		auto device = std::make_unique<md::Device>(params);
		auto& hardware = device->getHardware();
		advance(hardware, md::g_samplerate * 25);
		require(hardware.isAudioReady() && hardware.isFirmwareMidiReady(), "boot incomplete");
		md::PanelRowState rows;
		const auto send = [&](const md::PanelPacket packet, bool down)
		{
			const auto combined = down ? rows.press(packet) : rows.release(packet);
			require(hardware.trySendPanelEvent(combined.row, combined.mask), "panel event rejected");
			advance(hardware, md::g_samplerate / 8);
		};
		const auto tap = [&](md::PanelControl control)
		{
			const auto packet = md::panelPacket(model, control);
			require(packet.has_value(), "missing panel button");
			send(*packet, true); send(*packet, false);
			advance(hardware, md::g_samplerate / 2);
		};
		tap(md::PanelControl::Exit);
		if(model == md::MachineModel::Monomachine)
		{
			tap(md::PanelControl::DataPageForward);
			tap(md::PanelControl::DataPageForward); // FILTER: eight available parameters.
		}
		else
		{
			if(!hardware.getFrontPanelSnapshot().getModeLed(md::FrontPanel::ModeLed::Extended))
				tap(md::PanelControl::ClassicExtended);
			tap(md::PanelControl::SynthesisEffectsRouting); // EFFECTS.
		}
		tap(md::PanelControl::Record);
		tap(md::PanelControl::Trigger1);
		const auto trig = *md::panelPacket(model, md::PanelControl::Trigger1);
		send(trig, true);
		tap(md::PanelControl::Play); // Clear this step's locks, retaining its trig.
		const auto click = [&](unsigned encoder)
		{
			const auto packet = md::panelEncoderPressPacket(model, static_cast<md::PanelEncoder>(encoder));
			require(packet.has_value(), "missing encoder switch mapping");
			if(!dropPress) send(*packet, true);
			if(!dropRelease) send(*packet, false);
			advance(hardware, md::g_samplerate / 4);
		};
		for(unsigned i = 0; i < 8; ++i)
		{
			require(!locked(hardware, i), "initial parameter unexpectedly locked");
			click(i);
			require(locked(hardware, i), "encoder press did not create current-value lock");
			click(i);
			require(!locked(hardware, i), "encoder press did not clear lock");
		}
		click(0); click(7); click(0);
		require(!locked(hardware, 0) && locked(hardware, 7), "selective clear affected another parameter");
		click(7);
		const auto encoder = *md::panelEncoderPressPacket(model, md::PanelEncoder::DataEntryA);
		send(encoder, true);
		require(locked(hardware, 0), "held encoder did not lock parameter");
		send(encoder, true);
		require(locked(hardware, 0), "repeated held row toggled lock twice");
		const auto beforeTurn = hardware.getFrontPanelSnapshot();
		require(hardware.trySendPanelEvent(*md::panelEncoderCommand(model, md::PanelEncoder::DataEntryA), 1),
			"held encoder turn rejected");
		advance(hardware, md::g_samplerate / 2);
		const auto afterTurn = hardware.getFrontPanelSnapshot();
		bool changed = false;
		for(unsigned y = 14; y <= 30; ++y)
			for(unsigned x = 52; x <= 62; ++x)
				changed |= beforeTurn.getLcdPixel(x, y) != afterTurn.getLcdPixel(x, y);
		require(changed && locked(hardware, 0), "press-and-turn failed to edit held parameter");
		send(encoder, false);
		click(0);
		require(!locked(hardware, 0), "release failed to rearm the next encoder click");
		send(trig, false);
		std::cout << "PASS: all eight physical encoder switches create and selectively clear locks\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
