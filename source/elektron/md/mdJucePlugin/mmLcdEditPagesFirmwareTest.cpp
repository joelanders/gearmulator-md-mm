#include "mdLcdInteractionModel.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
constexpr auto model = md::MachineModel::Monomachine;
void require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
void advance(md::Hardware& hw, uint32_t frames) {
	for(uint32_t done=0; done<frames;) { const auto n=std::min<uint32_t>(256,frames-done); hw.advance(n); done+=n; }
}
void tap(md::Hardware& hw, md::PanelControl control) {
	const auto p=md::panelPacket(model,control); require(p.has_value(),"missing panel mapping");
	require(hw.trySendPanelEvent(p->row,p->mask),"panel press rejected"); advance(hw,2048);
	require(hw.trySendPanelEvent(p->row,0),"panel release rejected"); advance(hw,md::g_samplerate/2);
}
std::optional<unsigned> activePage(const md::FrontPanel& panel) {
	constexpr uint8_t banks[]{0x25,0x25,0x25,0x25,0x26,0x26,0x26};
	constexpr uint8_t bits[]{4,5,6,7,0,1,2}; std::optional<unsigned> result;
	for(unsigned page=0;page<7;++page) if((panel.getLedBankRaw(banks[page])&(1u<<bits[page]))==0) {
		if(result) return std::nullopt; result=page;
	} return result;
}
void selectPage(md::Hardware& hw, unsigned target) {
	for(unsigned attempt=0;attempt<8;++attempt) { const auto page=activePage(hw.getFrontPanelSnapshot());
		if(page&&*page==target) return; tap(hw,md::PanelControl::DataPageForward); }
	const auto panel=hw.getFrontPanelSnapshot();
	const auto page=activePage(panel);
	throw std::runtime_error("DATA page selection did not converge on page "+std::to_string(target+1)
		+" (current="+(page?std::to_string(*page+1):std::string{"ambiguous"})
		+", LED 25="+std::to_string(panel.getLedBankRaw(0x25))
		+", 26="+std::to_string(panel.getLedBankRaw(0x26))+", 27="
		+std::to_string(panel.getLedBankRaw(0x27))+")");
}
void verify(md::Hardware& hw) {
	for(unsigned track=0;track<6;++track) {
		tap(hw,static_cast<md::PanelControl>(static_cast<unsigned>(md::PanelControl::Track1)+track));
		for(unsigned page=0;page<7;++page) { selectPage(hw,page);
			const auto panel=hw.getFrontPanelSnapshot();
			const auto state=mdJucePlugin::lcdInteraction::classify(panel,model);
			if(!state) throw std::runtime_error("track "+std::to_string(track+1)+", EDIT page "+std::to_string(page+1)
				+" was not classified (LED 25="+std::to_string(panel.getLedBankRaw(0x25))
				+", 26="+std::to_string(panel.getLedBankRaw(0x26))+", 27="
				+std::to_string(panel.getLedBankRaw(0x27))+")");
			require(state->layout==mdJucePlugin::lcdInteraction::LayoutKind::Standard,"wrong EDIT geometry");
		}
	} selectPage(hw,0); tap(hw,md::PanelControl::Track1);
}
void loadEmptyKit(md::Hardware& hw) {
	tap(hw,md::PanelControl::Kit); tap(hw,md::PanelControl::Enter);
	const auto fn=md::panelPacket(model,md::PanelControl::Function), play=md::panelPacket(model,md::PanelControl::Play);
	require(fn&&play,"missing clear-kit controls"); md::PanelRowState rows;
	const std::array packets{rows.press(*fn),rows.press(*play),rows.release(*play),rows.release(*fn)};
	for(const auto p:packets) { require(hw.trySendPanelEvent(p.row,p.mask),"clear-kit rejected"); advance(hw,2048); }
	advance(hw,md::g_samplerate*2); tap(hw,md::PanelControl::Enter); tap(hw,md::PanelControl::Exit);
	// Give every track a real synthesis surface after the kit transition. Machine
	// 01 is GND-SIN and init=1 initializes every DATA page (MM MIDI spec, App. C).
	for(uint8_t track=0; track<6; ++track) {
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex={0xf0,0,0x20,0x3c,3,0,0x5b,track,1,1,0xf7};
		require(hw.sendMidi(assign),"machine assignment rejected");
		advance(hw,md::g_samplerate);
	}
}
}
int main() {
	const auto* path=std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path||!*path) { std::cout<<"mmLcdEditPagesFirmwareTest: SKIP (MM firmware not supplied)\n"; return 77; }
	try { std::vector<uint8_t> rom; require(baseLib::filesystem::readFile(rom,path),"could not read firmware");
		require(md::RomLoader::isRomForModel(rom,model),"firmware fingerprint mismatch");
		// Hardware exceeds the default Windows thread stack. Match the product's
		// heap ownership so this firmware test also runs with MSVC defaults.
		auto hardware=std::make_unique<md::Hardware>(rom,path,model);
		auto& hw=*hardware;
		advance(hw,md::g_samplerate*20);
		require(hw.isAudioReady()&&hw.isFirmwareMidiReady(),"boot incomplete after 20 seconds of emulated time");
		verify(hw); loadEmptyKit(hw); verify(hw);
		std::cout<<"mmLcdEditPagesFirmwareTest: PASS, 6 tracks x 7 EDIT pages before and after kit load\n"; return 0;
	} catch(const std::exception& e) { std::cerr<<"mmLcdEditPagesFirmwareTest: "<<e.what()<<'\n'; return 1; }
}
