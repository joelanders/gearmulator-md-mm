#pragma once

#include "mdLib/mdhardware.h"

#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <stdexcept>

namespace md::test
{
	inline void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}
	inline void advanceFrames(Hardware& hardware, uint32_t frames)
	{
		while(frames) { const auto n = std::min<uint32_t>(frames, 64); hardware.advance(n); frames -= n; }
	}
	inline void panelTap(Hardware& hardware, PanelControl control)
	{
		const auto packet = panelPacket(hardware.getModel(), control);
		require(packet.has_value(), "unknown panel control");
		require(hardware.trySendPanelEvent(packet->row, packet->mask), "panel press rejected");
		advanceFrames(hardware, 2048);
		require(hardware.trySendPanelEvent(packet->row, 0), "panel release rejected");
		advanceFrames(hardware, 6400);
	}
	inline void panelChord(Hardware& hardware, PanelControl control)
	{
		const auto function = panelPacket(hardware.getModel(), PanelControl::Function);
		const auto target = panelPacket(hardware.getModel(), control);
		require(function && target, "unknown panel chord");
		PanelRowState rows;
		const std::array packets{rows.press(*function), rows.press(*target), rows.release(*target), rows.release(*function)};
		for(const auto packet : packets)
		{
			require(hardware.trySendPanelEvent(packet.row, packet.mask), "panel chord rejected");
			advanceFrames(hardware, 2048);
		}
		advanceFrames(hardware, 6400);
	}
	inline void panelImage(const Hardware& hardware, const std::string& path)
	{
		const auto panel = hardware.getFrontPanelSnapshot();
		std::ofstream image(path, std::ios::binary);
		image << "P5\n128 64\n255\n";
		for(uint32_t y = 0; y < 64; ++y)
			for(uint32_t x = 0; x < 128; ++x) image.put(panel.getLcdPixel(x, y) ? '\0' : '\xff');
	}
	inline void enterMmReceive(Hardware& hardware, bool digiPro)
	{
		static unsigned entry = 0;
		const auto probe = [&hardware, base = entry++](unsigned stage)
		{
			if(const auto* prefix = std::getenv("MM_SYSEX_PANEL_PREFIX"))
				panelImage(hardware, std::string(prefix) + "-" + std::to_string(base) + "-" + std::to_string(stage) + ".pgm");
		};
		panelChord(hardware, PanelControl::Kit);
		probe(0);
		panelTap(hardware, PanelControl::Enter);
		probe(1);
		for(unsigned i = 0; i < 4; ++i) panelTap(hardware, PanelControl::Left);
		for(unsigned i = 0; i < 8; ++i) panelTap(hardware, PanelControl::Up);
		panelTap(hardware, PanelControl::Down);
		panelTap(hardware, PanelControl::Down);
		panelTap(hardware, PanelControl::Right);
		probe(2);
		for(unsigned i = 0; i < 8; ++i) panelTap(hardware, PanelControl::Up);
		panelTap(hardware, PanelControl::Down);
		if(digiPro) panelTap(hardware, PanelControl::Down);
		panelTap(hardware, PanelControl::Enter);
		probe(3);
		panelTap(hardware, PanelControl::Right);
		panelTap(hardware, PanelControl::Enter);
		probe(4);
	}
	inline void exitMenus(Hardware& hardware)
	{
		for(unsigned i = 0; i < 8; ++i) panelTap(hardware, PanelControl::Exit);
	}
	inline void emptyMmKit(Hardware& hardware)
	{
		exitMenus(hardware);
		panelTap(hardware, PanelControl::Kit);
		panelTap(hardware, PanelControl::Enter);
		panelChord(hardware, PanelControl::Play);
		advanceFrames(hardware, g_samplerate * 2);
		panelTap(hardware, PanelControl::Enter);
		panelTap(hardware, PanelControl::Exit);
	}
}
