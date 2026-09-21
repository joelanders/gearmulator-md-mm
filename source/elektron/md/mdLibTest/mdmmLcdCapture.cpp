// Captures LCD screens from the emulated machines and checks the tooltip tables against them.
// Not part of the test suite: it needs firmware images, so it is run by hand after table changes.
//
//   GEARMULATOR_MM_FIRMWARE_BIN=/path/mm.bin mdmmLcdCapture <dir> [mode]
//   GEARMULATOR_MD_FIRMWARE_BIN=/path/md.bin mdmmLcdCapture <dir> md [mode]
//
// Default (no mode): assign every machine over SysEx, capture each screen as a PBM, verify every
// transcribed label against the pixels, check that each machine name resolves and that every label
// the plugin can read has a help entry, then print the hash tables the help headers are built from.
//
// Modes:
//   values     Monomachine LFO 1: sweep the knobs that show a text value (TRIG, WAVE, MULT).
//   enums      every machine: sweep each knob whose value line shows text.
//   lfo        Monomachine LFO 1: sweep PAGE and, for each, DEST.
//   turncheck  turn every knob and confirm the labels never change (an LCD drag must not cancel).
//   screens    Machinedrum: capture the LFO window and the master FX windows.
//   mdlfo      Machinedrum LFO window: sweep UPDTE.
//   machines   Machinedrum: capture every machine id, including the UW set.
//   ccout      turn two knobs and print the MIDI the firmware sends.
#include "../mdJucePlugin/mdLcdText.h"
#include "../mdJucePlugin/mdLcdInteractionModel.h"
#include "../mdJucePlugin/mdMachinedrumHelp.h"

#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
	void advance(md::Hardware& _hardware, uint32_t _frames)
	{
		while(_frames)
		{
			const auto chunk = std::min<uint32_t>(256, _frames);
			_hardware.advance(chunk);
			_frames -= chunk;
		}
	}

	md::MachineModel g_model = md::MachineModel::Monomachine;

	void tap(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(g_model, _control);
		if(!packet)
			return;
		_hardware.trySendPanelEvent(packet->row, packet->mask);
		advance(_hardware, 2048);
		_hardware.trySendPanelEvent(packet->row, 0);
		advance(_hardware, md::g_samplerate / 4);
	}

	void capture(const md::Hardware& _hardware, const std::string& _path)
	{
		const auto panel = _hardware.getFrontPanelSnapshot();
		auto* const file = std::fopen(_path.c_str(), "w");
		if(!file)
			return;
		std::fprintf(file, "P1\n%u %u\n", md::FrontPanel::g_lcdWidth, md::FrontPanel::g_lcdHeight);
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
		{
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				std::fputc(panel.getLcdPixel(x, y) ? '1' : '0', file);
			std::fputc('\n', file);
		}
		std::fclose(file);
	}


	// "ccout": turn DATA ENTRY A and B on the boot kit and print the MIDI the firmware sends.
	int runCcOut(md::Hardware& _hardware, const md::MachineModel _model)
	{
		std::vector<synthLib::SMidiEvent> out;
		_hardware.readMidiOut(out);
		out.clear();
		for(unsigned encoder = 0; encoder < 2; ++encoder)
		{
			const auto command = md::panelEncoderCommand(_model, static_cast<md::PanelEncoder>(encoder));
			for(int step = 0; step < 3; ++step)
			{
				_hardware.trySendPanelEvent(*command, 0x01);
				advance(_hardware, 4096);
			}
		}
		advance(_hardware, md::g_samplerate / 2);
		_hardware.readMidiOut(out);
		int shown = 0;
		for(const auto& e : out)
		{
			if(e.sysex.empty() && e.a == 0xf8)
				continue;	// clock
			if(shown++ < 20)
				std::printf("midi out: %02x %02x %02x%s\n", e.a, e.b, e.c, e.sysex.empty() ? "" : " (sysex)");
		}
		std::printf("ccout: %zu events, %d non-clock\n", out.size(), shown);
		return 0;
	}

	// "enums <model>": for every machine, sweep each SYNTHESIS knob whose value line shows text
	// (numeric parameters leave it blank) and record every distinct reading, for transcription.
	int runEnumSweep(md::Hardware& _hardware, const md::MachineModel _model, const std::string& _out)
	{
		const auto assign = [&](const int _id, const bool _userWave)
		{
			synthLib::SMidiEvent e(synthLib::MidiEventSource::Host);
			if(_model == md::MachineModel::Monomachine)
				e.sysex = { 0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x5b, 0x00,
					static_cast<uint8_t>(_id), 0x01, 0xf7 };
			else
				e.sysex = { 0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b, 0x00,
					static_cast<uint8_t>(_id), static_cast<uint8_t>(_userWave ? 1 : 0), 0x02, 0xf7 };
			_hardware.sendMidi(e);
			advance(_hardware, md::g_samplerate);
		};
		const auto turn = [&](const unsigned _encoder, const int _steps)
		{
			const auto command = md::panelEncoderCommand(_model, static_cast<md::PanelEncoder>(_encoder));
			for(int i = 0; i < std::abs(_steps); ++i)
			{
				_hardware.trySendPanelEvent(*command, _steps > 0 ? 0x01 : 0xff);
				advance(_hardware, 1024);
			}
			advance(_hardware, md::g_samplerate / 16);
		};
		// Monomachine: every machine. Machinedrum: one machine per family, since the help tables
		// are per family (plus CTR-AL/8P and the master FX control machines, which differ).
		std::vector<int> keys;
		if(_model == md::MachineModel::Monomachine)
			keys = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,32,33 };
		else
			keys = { 1,3,16,17,19,22,24,25,28,32,36,38,48,49,56,57,63,64,68,72,80,82,84,96,112,113,120,121,122,123,0x100,0x120 };
		for(const auto key : keys)
		{
			assign(key & 0xff, (key & 0x100) != 0);
			for(unsigned encoder = 0; encoder < 8; ++encoder)
			{
				const auto valueRegion = mdJucePlugin::lcdText::lfoValue(encoder);
				if(mdJucePlugin::lcdText::blank(_hardware.getFrontPanelSnapshot(), valueRegion))
					continue;	// numeric parameter: the value line stays empty
				const auto read = [&]
				{
					return mdJucePlugin::lcdText::hash(_hardware.getFrontPanelSnapshot(), valueRegion);
				};
				// A value can span ~32 detents, so scan the whole range in coarse steps instead of
				// waiting for a change after each one.
				turn(encoder, -160);
				std::vector<uint64_t> seen;
				for(int scan = 0; scan <= 40 && seen.size() < 40; ++scan)
				{
					const auto now = read();
					if(std::find(seen.begin(), seen.end(), now) == seen.end())
					{
						std::printf("machine 0x%03x knob %u value %zu 0x%016llx\n", key, encoder,
							seen.size(), static_cast<unsigned long long>(now));
						capture(_hardware, _out + "/enum-" + std::to_string(key) + "-" + std::to_string(encoder)
							+ "-" + std::to_string(seen.size()) + ".pbm");
						seen.push_back(now);
					}
					turn(encoder, 4);
				}
				std::fprintf(stderr, "machine 0x%03x knob %u: %zu values\n", key, encoder, seen.size());
			}
		}
		return 0;
	}
	// Machinedrum OS 1.63 (UW): capture boot, the three data pages and every machine.
	int runMachinedrum(const std::string& _out, int argc, char** argv)
	{
		const auto* path = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
		std::vector<uint8_t> rom;
		if(!path || !baseLib::filesystem::readFile(rom, path)
			|| !md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum))
		{
			std::cerr << "firmware missing or not a Machinedrum image\n";
			return 2;
		}
		g_model = md::MachineModel::Machinedrum;
		auto hardware = std::make_unique<md::Hardware>(rom, path, md::MachineModel::Machinedrum);
		advance(*hardware, md::g_samplerate * 20);
		if(argc >= 4 && std::string(argv[3]) == "ccout")
			return runCcOut(*hardware, md::MachineModel::Machinedrum);
		// "mdlfo": open the LFO window (FUNCTION + SYNTHESIS/EFFECTS/ROUTING) and sweep UPDTE.
		if(argc >= 4 && std::string(argv[3]) == "mdlfo")
		{
			const auto packet = md::panelPacket(g_model, md::PanelControl::Function);
			hardware->trySendPanelEvent(packet->row, packet->mask);
			advance(*hardware, 4096);
			tap(*hardware, md::PanelControl::SynthesisEffectsRouting);
			hardware->trySendPanelEvent(packet->row, 0);
			advance(*hardware, md::g_samplerate / 2);
			capture(*hardware, _out + "/mdlfo.pbm");
			const auto command = md::panelEncoderCommand(md::MachineModel::Machinedrum, md::PanelEncoder::DataEntryE);
			const auto read = [&]
			{
				return mdJucePlugin::lcdText::hash(hardware->getFrontPanelSnapshot(),
					mdJucePlugin::lcdText::mdLfoValue(4));
			};
			for(int i = 0; i < 40; ++i)
			{
				hardware->trySendPanelEvent(*command, 0xff);
				advance(*hardware, 1024);
			}
			advance(*hardware, md::g_samplerate / 8);
			std::vector<uint64_t> seen;
			for(int scan = 0; scan <= 40 && seen.size() < 12; ++scan)
			{
				const auto now = read();
				if(std::find(seen.begin(), seen.end(), now) == seen.end())
				{
					std::printf("updte value %zu 0x%016llx\n", seen.size(), static_cast<unsigned long long>(now));
					capture(*hardware, _out + "/updte-" + std::to_string(seen.size()) + ".pbm");
					seen.push_back(now);
				}
				for(int i = 0; i < 2; ++i)
				{
					hardware->trySendPanelEvent(*command, 0x01);
					advance(*hardware, 1024);
				}
				advance(*hardware, md::g_samplerate / 32);
			}
			return 0;
		}
		if(argc >= 4 && std::string(argv[3]) == "enums")
			return runEnumSweep(*hardware, md::MachineModel::Machinedrum, _out);

		capture(*hardware, _out + "/boot.pbm");
		for(int page = 1; page <= 2; ++page)
		{
			tap(*hardware, md::PanelControl::SynthesisEffectsRouting);
			capture(*hardware, _out + "/page-" + std::to_string(page) + ".pbm");
		}
		tap(*hardware, md::PanelControl::SynthesisEffectsRouting);
		capture(*hardware, _out + "/page-0.pbm");

		const auto assign = [&](const int _id, const bool _userWave)
		{
			// Manual, Appendix C: $5b assign machine -- track, machine, 1 = UW set, 2 = init all pages.
			synthLib::SMidiEvent e(synthLib::MidiEventSource::Host);
			e.sysex = { 0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b, 0x00, static_cast<uint8_t>(_id),
				static_cast<uint8_t>(_userWave ? 1 : 0), 0x02, 0xf7 };
			hardware->sendMidi(e);
			advance(*hardware, md::g_samplerate);
		};

		if(argc >= 4 && std::string(argv[3]) == "machines")
		{
			for(int id = 0; id < 128; ++id)
			{
				assign(id, false);
				capture(*hardware, _out + "/m-" + std::to_string(id) + ".pbm");
			}
			for(int id = 0; id < 64; ++id)
			{
				assign(id, true);
				capture(*hardware, _out + "/uw-" + std::to_string(id) + ".pbm");
			}
			return 0;
		}

		int errors = 0;
		std::vector<std::pair<uint64_t, std::string>> labelHashes;
		const auto verifyLabels = [&](const md::FrontPanel& _panel, const char* const* _labels, const std::string& _context)
		{
			for(unsigned field = 0; field < 8; ++field)
			{
				const auto region = mdJucePlugin::lcdText::fieldLabel(field);
				const std::string label = _labels[field];
				const auto isBlank = mdJucePlugin::lcdText::blank(_panel, region);
				if(label.empty() != isBlank)
				{
					std::cerr << _context << " field " << field << ": transcription '" << label
						<< "' but region is " << (isBlank ? "blank" : "not blank") << '\n';
					++errors;
					continue;
				}
				if(isBlank)
					continue;
				const auto hash = mdJucePlugin::lcdText::inkHash(_panel, region);
				const auto it = std::find_if(labelHashes.begin(), labelHashes.end(),
					[&](const auto& _e) { return _e.first == hash || _e.second == label; });
				if(it == labelHashes.end())
					labelHashes.emplace_back(hash, label);
				else if(it->first != hash || it->second != label)
				{
					std::cerr << _context << " field " << field << ": '" << label << "' conflicts with '"
						<< it->second << "' (same " << (it->first == hash ? "pixels" : "text, different pixels") << ")\n";
					++errors;
				}
			}
		};

		// The plugin's help must describe every label it can read, and resolve the machine name.
		const auto checkHelp = [&](const md::FrontPanel& _panel, const int _page, const int _key, const std::string& _context)
		{
			namespace help = mdJucePlugin::machinedrumHelp;
			uint16_t found = 0xffff;
			const auto* const machine = help::machineForHash(
				mdJucePlugin::lcdText::hash(_panel, mdJucePlugin::lcdText::g_mdMachineName), found);
			if(_key >= 0 && (!machine || found != _key))
			{
				std::cerr << _context << ": machine name does not resolve to its key\n";
				++errors;
			}
			// A page whose fields are all blank (CTR-RE/GB/EQ/DX EFFECTS) has nothing to edit.
			bool anyLabel = false;
			for(unsigned field = 0; field < 8; ++field)
				anyLabel = anyLabel || !mdJucePlugin::lcdText::blank(_panel, mdJucePlugin::lcdText::fieldLabel(field));
			if(_page > 0 && anyLabel
				&& !mdJucePlugin::lcdInteraction::classify(_panel, md::MachineModel::Machinedrum, false))
			{
				std::cerr << _context << ": not recognised as an editable screen\n";
				++errors;
			}
			for(unsigned field = 0; field < 8; ++field)
			{
				const auto region = mdJucePlugin::lcdText::fieldLabel(field);
				if(mdJucePlugin::lcdText::blank(_panel, region))
					continue;
				bool allTracks = false;
				if(!help::entry(_page, machine, help::labelForHash(mdJucePlugin::lcdText::inkHash(_panel, region)), allTracks))
				{
					std::cerr << _context << " field " << field << ": no help entry\n";
					++errors;
				}
			}
		};
		// TRACK EFFECTS and ROUTING on the boot kit, transcribed from the captures.
		constexpr const char* pageLabels[2][8] =
		{
			{ "AMD", "AMF", "EQF", "EQG", "FLTF", "FLTW", "FLTQ", "SRR" },
			{ "DIST", "VOL", "PAN", "DEL", "REV", "LFOS", "LFOD", "LFOM" },
		};
		std::vector<md::FrontPanel> pagePanels;
		for(int page = 1; page <= 2; ++page)
		{
			tap(*hardware, md::PanelControl::SynthesisEffectsRouting);
			pagePanels.push_back(hardware->getFrontPanelSnapshot());
			verifyLabels(pagePanels.back(), pageLabels[page - 1], "page " + std::to_string(page));
			checkHelp(pagePanels.back(), page, -1, "page " + std::to_string(page));
		}
		// A popup over a data page must break the field frame, so no field stays draggable under it.
		tap(*hardware, md::PanelControl::Kit);
		if(mdJucePlugin::lcdInteraction::classify(hardware->getFrontPanelSnapshot(), md::MachineModel::Machinedrum, false))
		{
			std::cerr << "KIT menu over ROUTING is still treated as editable\n";
			++errors;
		}
		tap(*hardware, md::PanelControl::Exit);
		tap(*hardware, md::PanelControl::SynthesisEffectsRouting);

		// "screens": the LFO edit window (FUNCTION + SYNTHESIS/EFFECTS/ROUTING) and the KIT menu.
		if(argc >= 4 && std::string(argv[3]) == "screens")
		{
			const auto hold = [&](const md::PanelControl _held, const md::PanelControl _tapped)
			{
				const auto packet = md::panelPacket(g_model, _held);
				hardware->trySendPanelEvent(packet->row, packet->mask);
				advance(*hardware, 4096);
				tap(*hardware, _tapped);
				hardware->trySendPanelEvent(packet->row, 0);
				advance(*hardware, md::g_samplerate / 2);
			};
			hold(md::PanelControl::Function, md::PanelControl::SynthesisEffectsRouting);
			capture(*hardware, _out + "/lfo.pbm");
			tap(*hardware, md::PanelControl::Exit);
			tap(*hardware, md::PanelControl::Kit);
			tap(*hardware, md::PanelControl::Down);
			tap(*hardware, md::PanelControl::Right);
			tap(*hardware, md::PanelControl::Enter);
			for(int i = 0; i < 4; ++i)
			{
				capture(*hardware, _out + "/master-" + std::to_string(i) + ".pbm");
				tap(*hardware, md::PanelControl::Right);
			}
			return 0;
		}

		// "turncheck": on both pages, turn every knob and check no label changes after a detent.
		// Also on MID-01 and CTR-AL, whose EFFECTS/ROUTING pages show their own labels.
		if(argc >= 4 && std::string(argv[3]) == "turncheck")
		{
			int hidden = 0, checks = 0;
			for(int round = 0; round < 3; ++round)
			for(int page = 1; page <= 2; ++page)
			{
				if(page == 1 && round == 1)
					assign(96, false);
				else if(page == 1 && round == 2)
					assign(112, false);
				tap(*hardware, md::PanelControl::SynthesisEffectsRouting);
				const auto before = hardware->getFrontPanelSnapshot();
				std::vector<uint64_t> expected(8);
				for(unsigned field = 0; field < 8; ++field)
					expected[field] = mdJucePlugin::lcdText::hash(before, mdJucePlugin::lcdText::fieldLabel(field));
				for(unsigned encoder = 0; encoder < 8; ++encoder)
				{
					const auto command = md::panelEncoderCommand(md::MachineModel::Machinedrum,
						static_cast<md::PanelEncoder>(encoder));
					for(int step = 0; step < 6; ++step)
					{
						hardware->trySendPanelEvent(*command, step < 3 ? 0x01 : 0xff);
						advance(*hardware, 2048);
						const auto now = hardware->getFrontPanelSnapshot();
						++checks;
						for(unsigned field = 0; field < 8; ++field)
						{
							if(mdJucePlugin::lcdText::hash(now, mdJucePlugin::lcdText::fieldLabel(field)) != expected[field])
							{
								std::printf("round %d page %d knob %u step %d: field %u label changed\n", round, page, encoder, step, field);
								++hidden;
								capture(*hardware, _out + "/turn-p" + std::to_string(page) + "-k" + std::to_string(encoder)
									+ "-s" + std::to_string(step) + ".pbm");
								break;
							}
						}
					}
				}
				if(page == 2)
					tap(*hardware, md::PanelControl::SynthesisEffectsRouting);	// back to SYNTHESIS
			}
			std::printf("turncheck: %d label disruptions in %d checks\n", hidden, checks);
			return 0;
		}

		// Every machine in Appendix C. Key: SysEx number, plus 0x100 for the UW set (c = 1).
		std::vector<int> keys;
		for(const auto& [first, last] : std::initializer_list<std::pair<int, int>>{
			{0, 3}, {16, 28}, {32, 39}, {48, 72}, {80, 85}, {96, 113}, {120, 123} })
			for(int id = first; id <= last; ++id)
				keys.push_back(id);
		for(const auto& [first, last] : std::initializer_list<std::pair<int, int>>{ {0, 35}, {37, 40}, {48, 63} })
			for(int id = first; id <= last; ++id)
				keys.push_back(0x100 | id);

		// SYNTHESIS labels transcribed from the captures (A-D, E-H; "" empty), checked against
		// Appendix A. Labels drawn inverted (white on black) are written in brackets. Machines
		// whose TRACK EFFECTS / ROUTING pages show their own labels list those too.
		using Labels = const char* [8];
		constexpr Labels mid1{ "CC1D", "CC1V", "CC2D", "CC2V", "CC3D", "CC3V", "CC4D", "CC4V" };
		constexpr Labels mid2{ "CC5D", "CC5V", "CC6D", "CC6V", "PCHG", "LFOS", "LFOD", "LFOM" };
		constexpr Labels ctrAl1{ "[AMD]", "[AMF]", "[EQF]", "[EQG]", "[FLTF]", "[FLTW]", "[FLTQ]", "[SRR]" };
		constexpr Labels ctrAl2{ "[DIST]", "[VOL]", "[PAN]", "[DEL]", "[REV]", "[LFOS]", "[LFOD]", "[LFOM]" };
		constexpr Labels ctrFx1{ "", "", "", "", "", "", "", "" };
		constexpr Labels ctrFx2{ "", "", "", "", "", "LFOS", "LFOD", "LFOM" };
		struct Transcription
		{
			int first, last;
			Labels synthesis;
			const char* const* effects;
			const char* const* routing;
		};
		const Transcription transcriptions[] =
		{
			{ 0, 0, { "", "", "", "", "", "", "", "" } },
			{ 1, 1, { "PTCH", "DEC", "RAMP", "RDEC", "", "", "", "" } },
			{ 2, 2, { "DEC", "", "", "", "", "", "", "" } },
			{ 3, 3, { "UP", "UVAL", "DOWN", "DVAL", "", "", "", "" } },
			{ 16, 16, { "PTCH", "DEC", "RAMP", "RDEC", "STRT", "NOIS", "HARM", "CLIP" } },
			{ 17, 17, { "PTCH", "DEC", "BUMP", "BENV", "SNAP", "TONE", "TUNE", "CLIP" } },
			{ 18, 18, { "PTCH", "DEC", "RAMP", "RDEC", "DAMP", "DIST", "DTYP", "" } },
			{ 19, 19, { "CLPY", "TONE", "HARD", "RICH", "RATE", "ROOM", "RSIZ", "RTUN" } },
			{ 20, 20, { "PTCH", "DEC", "DIST", "", "", "", "", "" } },
			{ 21, 21, { "PTCH", "DEC", "ENH", "DAMP", "TONE", "BUMP", "", "" } },
			{ 22, 23, { "GAP", "DEC", "HPF", "LPF", "MTAL", "", "", "" } },
			{ 24, 24, { "RICH", "DEC", "TOP", "TTUN", "SIZE", "PEAK", "", "" } },
			{ 25, 25, { "ATT", "SUS", "REV", "DAMP", "RATL", "RTYP", "TONE", "HARD" } },
			{ 26, 26, { "PTCH", "DEC", "DUAL", "ENH", "TUNE", "CLIC", "", "" } },
			{ 27, 27, { "PTCH", "DEC", "RAMP", "RDEC", "DAMP", "DIST", "DTYP", "" } },
			{ 28, 28, { "PTCH", "DEC", "RAMP", "HOLD", "TICK", "NOIS", "DIRT", "DIST" } },
			{ 32, 32, { "PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "MFB" } },
			{ 33, 33, { "PTCH", "DEC", "NOIS", "NDEC", "MOD", "MFRQ", "MDEC", "HPF" } },
			{ 34, 34, { "PTCH", "DEC", "RAMP", "RDEC", "MOD", "MFRQ", "MDEC", "CLIC" } },
			{ 35, 35, { "PTCH", "DEC", "CLPS", "CDEC", "MOD", "MFRQ", "MDEC", "HPF" } },
			{ 36, 36, { "PTCH", "DEC", "MOD", "HPF", "SNAR", "SPTC", "SDEC", "SMOD" } },
			{ 37, 37, { "PTCH", "DEC", "SNAP", "FB", "MOD", "MFRQ", "MDEC", "" } },
			{ 38, 38, { "PTCH", "DEC", "TREM", "TFRQ", "MOD", "MFRQ", "MDEC", "FB" } },
			{ 39, 39, { "PTCH", "DEC", "FB", "HPF", "MOD", "MFRQ", "MDEC", "" } },
			{ 48, 48, { "PTCH", "DEC", "SNAP", "SPLN", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 49, 49, { "PTCH", "DEC", "HP", "RING", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 50, 52, { "PTCH", "DEC", "HP", "HPQ", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 53, 53, { "PTCH", "DEC", "HP", "RATL", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 54, 55, { "PTCH", "DEC", "HP", "HPQ", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 56, 56, { "PTCH", "DEC", "HP", "STOP", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 57, 57, { "PTCH", "DEC", "HP", "BELL", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 58, 58, { "PTCH", "DEC", "HP", "HPQ", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 59, 59, { "PTCH", "DEC", "HP", "REAL", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 60, 61, { "PTCH", "DEC", "HP", "HPQ", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 62, 62, { "PTCH", "DEC", "HP", "SLEW", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 63, 63, { "PTCH", "DEC", "HP", "BC", "STRT", "RTRG", "RTIM", "BEND" } },
			{ 64, 64, { "PTCH", "DEC", "HARD", "HAMR", "TENS", "DAMP", "", "" } },
			{ 65, 65, { "PTCH", "DEC", "HARD", "RING", "TENS", "RVOL", "RDEC", "" } },
			{ 66, 66, { "PTCH", "DEC", "HARD", "HAMR", "TUNE", "DAMP", "SIZE", "POS" } },
			{ 67, 67, { "PTCH", "DEC", "HARD", "TENS", "", "", "", "" } },
			{ 68, 68, { "GRNS", "DEC", "GLEN", "", "SIZE", "HARD", "", "" } },
			{ 69, 69, { "PTCH", "DEC", "HARD", "RING", "RVOL", "RDEC", "", "" } },
			{ 70, 71, { "PTCH", "DEC", "HARD", "RING", "AG", "AU", "BR", "GRAB" } },
			{ 72, 72, { "PTCH", "DEC", "CLSN", "RING", "AG", "AU", "BR", "CLOS" } },
			{ 80, 81, { "VOL", "GATE", "ATCK", "HLD", "DEC", "", "", "" } },
			{ 82, 83, { "ALEV", "GATE", "FATK", "FHLD", "FDEC", "FDPH", "FFRQ", "FQ" } },
			{ 84, 85, { "ALEV", "AHLD", "ADEC", "FQ", "FDPH", "FHLD", "FDEC", "FFRQ" } },
			{ 96, 111, { "NOTE", "N2", "N3", "LEN", "VEL", "PB", "MW", "AT" }, mid1, mid2 },
			{ 112, 112, { "[SYN1]", "[SYN2]", "[SYN3]", "[SYN4]", "[SYN5]", "[SYN6]", "[SYN7]", "[SYN8]" }, ctrAl1, ctrAl2 },
			{ 113, 113, { "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8" } },
			{ 120, 120, { "TIME", "MOD", "MFRQ", "FB", "FLTF", "FLTW", "MONO", "LEV" }, ctrFx1, ctrFx2 },
			{ 121, 121, { "DVOL", "PRED", "DEC", "DAMP", "HP", "LP", "GATE", "LEV" }, ctrFx1, ctrFx2 },
			{ 122, 122, { "LF", "LG", "HF", "HG", "PF", "PG", "PQ", "GAIN" }, ctrFx1, ctrFx2 },
			{ 123, 123, { "ATCK", "REL", "TRHD", "RTIO", "KNEE", "HP", "OUTG", "MIX" }, ctrFx1, ctrFx2 },
			{ 0x100, 0x11f, { "PTCH", "DEC", "HOLD", "BRR", "STRT", "END", "RTRG", "RTIM" } },
			{ 0x120, 0x121, { "MLEV", "MBAL", "ILEV", "IBAL", "CUE1", "CUE2", "LEN", "RATE" } },
			{ 0x122, 0x123, { "PTCH", "DEC", "HOLD", "BRR", "STRT", "END", "RTRG", "RTIM" } },
			{ 0x125, 0x126, { "MLEV", "MBAL", "ILEV", "IBAL", "CUE1", "CUE2", "LEN", "RATE" } },
			{ 0x127, 0x128, { "PTCH", "DEC", "HOLD", "BRR", "STRT", "END", "RTRG", "RTIM" } },
			{ 0x130, 0x13f, { "PTCH", "DEC", "HOLD", "BRR", "STRT", "END", "RTRG", "RTIM" } },
		};

		std::vector<std::pair<uint64_t, int>> machineHashes;
		for(const auto key : keys)
		{
			assign(key & 0xff, (key & 0x100) != 0);
			const auto name = [&]
			{
				return mdJucePlugin::lcdText::hash(hardware->getFrontPanelSnapshot(), mdJucePlugin::lcdText::g_mdMachineName);
			};
			const auto nameHash = name();
			capture(*hardware, _out + "/k-" + std::to_string(key) + ".pbm");

			const auto* transcription = [&]() -> const Transcription*
			{
				for(const auto& t : transcriptions)
					if(key >= t.first && key <= t.last)
						return &t;
				return nullptr;
			}();
			if(!transcription)
			{
				std::cerr << "machine " << key << " has no transcription\n";
				++errors;
			}
			else
			{
				verifyLabels(hardware->getFrontPanelSnapshot(), transcription->synthesis, "machine " + std::to_string(key));
				checkHelp(hardware->getFrontPanelSnapshot(), 0, key, "machine " + std::to_string(key));
			}

			// The strip also shows the page (SYNT/TFX/ROUT); the machine part must not change with it.
			for(int page = 1; page <= 3; ++page)
			{
				tap(*hardware, md::PanelControl::SynthesisEffectsRouting);
				if(page < 3)
					capture(*hardware, _out + "/k-" + std::to_string(key) + "-p" + std::to_string(page) + ".pbm");
				if(transcription && page < 3)
				{
					const auto* const own = page == 1 ? transcription->effects : transcription->routing;
					// CTR-8P pairs track and parameter fields per shortcut; it is not transcribed.
					if(own || key != 113)
					{
						verifyLabels(hardware->getFrontPanelSnapshot(), own ? own : pageLabels[page - 1],
							"machine " + std::to_string(key) + " page " + std::to_string(page));
						checkHelp(hardware->getFrontPanelSnapshot(), page, key,
							"machine " + std::to_string(key) + " page " + std::to_string(page));
					}
				}
				if(name() != nameHash)
				{
					std::cerr << "machine " << key << " name hash changes on page " << page << '\n';
					++errors;
				}
			}
			for(const auto& [hash, other] : machineHashes)
			{
				if(hash == nameHash)
				{
					std::cerr << "machine " << key << " name looks identical to machine " << other << '\n';
					++errors;
				}
			}
			machineHashes.emplace_back(nameHash, key);
		}

		std::printf("// Generated by mdmmLcdCapture from Machinedrum OS 1.63 screens.\n");
		for(const auto& [hash, label] : labelHashes)
			std::printf("{ 0x%016llxull, \"%s\" },\n", static_cast<unsigned long long>(hash), label.c_str());
		std::printf("// machine names\n");
		for(const auto& [hash, key] : machineHashes)
			std::printf("{ 0x%016llxull, 0x%03x },\n", static_cast<unsigned long long>(hash), key);
		std::cerr << "captured to " << _out << ", " << labelHashes.size() << " distinct labels, " << errors << " errors\n";
		return errors == 0 ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
	if(argc >= 3 && std::string(argv[2]) == "md")
		return runMachinedrum(argv[1], argc, argv);

	const auto* path = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(argc < 2 || !path)
	{
		std::cerr << "usage: GEARMULATOR_MM_FIRMWARE_BIN=... mdmmLcdCapture <dir>\n";
		return 2;
	}
	const std::string out = argv[1];

	std::vector<uint8_t> rom;
	if(!baseLib::filesystem::readFile(rom, path)
		|| !md::RomLoader::isRomForModel(rom, md::MachineModel::Monomachine))
	{
		std::cerr << "firmware missing or not a Monomachine image\n";
		return 2;
	}

	auto hardware = std::make_unique<md::Hardware>(rom, path, md::MachineModel::Monomachine);
	advance(*hardware, md::g_samplerate * 20);
	if(argc >= 3 && std::string(argv[2]) == "ccout")
		return runCcOut(*hardware, md::MachineModel::Monomachine);
	if(argc >= 3 && std::string(argv[2]) == "enums")
		return runEnumSweep(*hardware, md::MachineModel::Monomachine, out);

	// "turncheck" mode: on AMP and LFO 1, turn each knob and read every field label right
	// after each detent, to see whether turning ever hides labels (which would end an LCD drag).
	if(argc >= 3 && std::string(argv[2]) == "turncheck")
	{
		std::vector<uint64_t> expected(8);
		int hidden = 0, checks = 0;
		for(int page = 1; page <= 4; ++page)
		{
			tap(*hardware, md::PanelControl::DataPageForward);
			if(page != 1 && page != 4)
				continue;
			const auto before = hardware->getFrontPanelSnapshot();
			for(unsigned field = 0; field < 8; ++field)
				expected[field] = mdJucePlugin::lcdText::hash(before, mdJucePlugin::lcdText::fieldLabel(field));
			for(unsigned encoder = 0; encoder < 8; ++encoder)
			{
				const auto command = md::panelEncoderCommand(md::MachineModel::Monomachine,
					static_cast<md::PanelEncoder>(encoder));
				for(int step = 0; step < 6; ++step)
				{
					hardware->trySendPanelEvent(*command, step < 3 ? 0x01 : 0xff);
					advance(*hardware, 2048);
					const auto now = hardware->getFrontPanelSnapshot();
					++checks;
					for(unsigned field = 0; field < 8; ++field)
					{
						if(mdJucePlugin::lcdText::hash(now, mdJucePlugin::lcdText::fieldLabel(field)) != expected[field])
						{
							std::printf("page %d knob %u step %d: field %u label changed\n", page, encoder, step, field);
							++hidden;
							capture(*hardware, out + "/turn-p" + std::to_string(page) + "-k" + std::to_string(encoder)
								+ "-s" + std::to_string(step) + ".pbm");
							break;
						}
					}
				}
			}
		}
		std::printf("turncheck: %d label disruptions in %d checks\n", hidden, checks);
		return 0;
	}

	// "values" mode: on MM LFO 1, sweep one knob at a time through its whole range and record every
	// distinct value-line reading, so enum values (TRIG, WAVE, ...) can be transcribed and hashed.
	if(argc >= 3 && std::string(argv[2]) == "values")
	{
		const auto turn = [&](const md::PanelEncoder _encoder, const int _steps)
		{
			const auto command = md::panelEncoderCommand(md::MachineModel::Monomachine, _encoder);
			for(int i = 0; i < std::abs(_steps); ++i)
			{
				hardware->trySendPanelEvent(*command, _steps > 0 ? 0x01 : 0xff);
				advance(*hardware, 1024);
			}
			advance(*hardware, md::g_samplerate / 8);
		};
		for(int page = 0; page < 4; ++page)
			tap(*hardware, md::PanelControl::DataPageForward);	// LFO 1
		for(const unsigned encoder : {2u, 3u, 4u, 6u})		// TRIG, WAVE, MULT, INTL
		{
			const auto knob = static_cast<md::PanelEncoder>(encoder);
			const auto value = [&]
			{
				return mdJucePlugin::lcdText::hash(hardware->getFrontPanelSnapshot(),
					mdJucePlugin::lcdText::lfoValue(encoder));
			};
			turn(knob, -140);
			std::vector<uint64_t> seen;
			for(int attempt = 0, unchanged = 0; attempt < 160 && unchanged < 4; ++attempt)
			{
				const auto now = value();
				if(!seen.empty() && now == seen.back())
				{
					++unchanged;
					turn(knob, 1);
					continue;
				}
				unchanged = 0;
				std::printf("knob %u value %zu 0x%016llx\n", encoder, seen.size(),
					static_cast<unsigned long long>(now));
				capture(*hardware, out + "/val-k" + std::to_string(encoder) + "-"
					+ std::to_string(seen.size()) + ".pbm");
				seen.push_back(now);
				turn(knob, 1);
			}
		}
		return 0;
	}
	// "lfo" mode: on LFO 1, step PAGE (knob A) through every value and, for each, DEST (knob B)
	// through every value, saving screens and value-line hashes for transcription.
	if(argc >= 3 && std::string(argv[2]) == "lfo")
	{
		const auto turn = [&](const md::PanelEncoder _encoder, const int _steps)
		{
			const auto command = md::panelEncoderCommand(md::MachineModel::Monomachine, _encoder);
			for(int i = 0; i < std::abs(_steps); ++i)
			{
				hardware->trySendPanelEvent(*command, _steps > 0 ? 0x01 : 0xff);
				advance(*hardware, 1024);
			}
			advance(*hardware, md::g_samplerate / 4);
		};
		const auto valueHash = [&](const unsigned _field)
		{
			auto region = mdJucePlugin::lcdText::fieldLabel(_field);
			region.y += 23;	// the value line sits 23 rows below the label on the LFO pages
			return mdJucePlugin::lcdText::hash(hardware->getFrontPanelSnapshot(), region);
		};

		for(int page = 0; page < 4; ++page)
			tap(*hardware, md::PanelControl::DataPageForward);

		// The firmware can swallow the first detent after a change of direction, so a value is
		// only treated as the last one after several single steps leave it unchanged.
		turn(md::PanelEncoder::DataEntryA, -40);
		std::vector<uint64_t> pages;
		for(int attempt = 0, unchanged = 0; attempt < 64 && unchanged < 4; ++attempt)
		{
			const auto pageHash = valueHash(0);
			if(!pages.empty() && pageHash == pages.back())
			{
				++unchanged;
				turn(md::PanelEncoder::DataEntryA, 1);
				continue;
			}
			unchanged = 0;
			const auto p = static_cast<int>(pages.size());
			pages.push_back(pageHash);
			capture(*hardware, out + "/lfo-p" + std::to_string(p) + ".pbm");
			std::printf("page %d 0x%016llx\n", p, static_cast<unsigned long long>(pageHash));

			turn(md::PanelEncoder::DataEntryB, -40);
			std::vector<uint64_t> dests;
			for(int dAttempt = 0, dUnchanged = 0; dAttempt < 64 && dUnchanged < 4; ++dAttempt)
			{
				const auto destHash = valueHash(1);
				if(!dests.empty() && destHash == dests.back())
				{
					++dUnchanged;
					turn(md::PanelEncoder::DataEntryB, 1);
					continue;
				}
				dUnchanged = 0;
				const auto d = static_cast<int>(dests.size());
				dests.push_back(destHash);
				capture(*hardware, out + "/lfo-p" + std::to_string(p) + "-d" + std::to_string(d) + ".pbm");
				std::printf("  dest %d 0x%016llx\n", d, static_cast<unsigned long long>(destHash));
				turn(md::PanelEncoder::DataEntryB, 1);
			}
			turn(md::PanelEncoder::DataEntryA, 1);
		}
		return 0;
	}

	capture(*hardware, out + "/boot.pbm");
	std::vector<md::FrontPanel> pagePanels;
	for(int page = 1; page <= 6; ++page)
	{
		tap(*hardware, md::PanelControl::DataPageForward);
		capture(*hardware, out + "/page-" + std::to_string(page) + ".pbm");
		pagePanels.push_back(hardware->getFrontPanelSnapshot());
	}
	for(int page = 0; page < 6; ++page)
		tap(*hardware, md::PanelControl::DataPageBackward);
	capture(*hardware, out + "/page-0.pbm");

	// SYNTHESIS page labels transcribed from these captures, checked against Appendix A of
	// the owner's manual. Order is fields A-D then E-H; "" is an empty field.
	struct Machine
	{
		int id;
		const char* labels[8];
	};
	constexpr Machine machines[] =
	{
		{ 0, { "", "", "", "", "", "", "", "" } },
		{ 1, { "", "", "", "", "", "", "", "TUNE" } },
		{ 2, { "ST", "RED", "STON", "", "", "", "", "TUNE" } },
		{ 3, { "PW", "PWAD", "PWRS", "WAVE", "MOD", "MSRC", "MFRQ", "TUNE" } },
		{ 4, { "UNIL", "UNIW", "UNIX", "", "SUBX", "SUB1", "SUB2", "TUNE" } },
		{ 5, { "UNIL", "UNIW", "SUB1", "SUB2", "PW", "PWAD", "PWRS", "TUNE" } },
		{ 6, { "WAVE", "WP", "WPM", "WPRS", "SYNC", "SFRQ", "", "TUNE" } },
		{ 7, { "PTCH", "STRT", "", "", "RTRG", "RTIM", "", "" } },
		{ 8, { "1FRQ", "1FIN", "1ENV", "1FB", "2FRQ", "2VOL", "TONE", "TUNE" } },
		{ 9, { "1FRQ", "1ENV", "2FRQ", "2ENV", "3FRQ", "3ENV", "TONE", "TUNE" } },
		{ 10, { "1FRQ", "1FEN", "1VOL", "1VEN", "2FRQ", "2ENV", "2FB", "TUNE" } },
		{ 11, { "VOC1", "VOC2", "V-SW", "VOIC", "CONS", "CLEN", "CVOL", "TUNE" } },
		{ 12, { "", "", "", "", "", "", "", "INP" } },
		{ 13, { "DEC", "DAMP", "GATE", "MIX", "HP", "LP", "", "INP" } },
		{ 14, { "PCH2", "PCH3", "PCH4", "WAVE", "PW", "CHRL", "CHRW", "TUNE" } },
		{ 15, { "DEL", "DEP", "SPD", "MIX", "FB", "WID", "LP", "INP" } },
		{ 16, { "ATK", "REL", "THRS", "MIX", "RAT", "GAIN", "RMS", "INP" } },
		{ 17, { "WAVE", "EXT", "", "MIX", "", "", "", "INP" } },
		{ 18, { "CNTR", "DEP", "SPD", "MIX", "FB", "WID", "", "INP" } },
		{ 19, { "DEL", "DEP", "SPD", "MIX", "FB", "WID", "", "INP" } },
		{ 32, { "WAV1", "MIX", "WAV2", "TIME", "BR1", "WID", "BR2", "TUNE" } },
		{ 33, { "PCH2", "PCH3", "PCH4", "WAVE", "", "CHRL", "CHRW", "TUNE" } },
	};

	std::vector<std::pair<uint64_t, std::string>> labelHashes;
	std::vector<std::pair<uint64_t, int>> machineHashes;
	int errors = 0;

	const auto verifyLabels = [&](const md::FrontPanel& _panel, const char* const* _labels, const std::string& _context)
	{
		for(unsigned field = 0; field < 8; ++field)
		{
			const auto region = mdJucePlugin::lcdText::fieldLabel(field);
			const std::string label = _labels[field];
			const auto isBlank = mdJucePlugin::lcdText::blank(_panel, region);
			if(label.empty() != isBlank)
			{
				std::cerr << _context << " field " << field << ": transcription '" << label
					<< "' but region is " << (isBlank ? "blank" : "not blank") << '\n';
				++errors;
				continue;
			}
			if(isBlank)
				continue;
			const auto hash = mdJucePlugin::lcdText::hash(_panel, region);
			const auto it = std::find_if(labelHashes.begin(), labelHashes.end(),
				[&](const auto& _e) { return _e.first == hash || _e.second == label; });
			if(it == labelHashes.end())
				labelHashes.emplace_back(hash, label);
			else if(it->first != hash || it->second != label)
			{
				std::cerr << _context << " field " << field << ": '" << label << "' conflicts with '"
					<< it->second << "' (same " << (it->first == hash ? "pixels" : "text, different pixels") << ")\n";
				++errors;
			}
		}
	};

	// Fixed DATA pages 1-6 on the boot kit, transcribed from the captures.
	constexpr const char* pageLabels[6][8] =
	{
		{ "ATK", "HOLD", "DEC", "REL", "DIST", "VOL", "PAN", "PORT" },
		{ "BASE", "WDTH", "HPQ", "LPQ", "ATK", "DEC", "BOFS", "WOFS" },
		{ "EQF", "EQG", "SRR", "DTIM", "DSND", "DFB", "DBAS", "DWID" },
		{ "PAGE", "DEST", "TRIG", "WAVE", "MULT", "SPD", "INTL", "DPTH" },
		{ "PAGE", "DEST", "TRIG", "WAVE", "MULT", "SPD", "INTL", "DPTH" },
		{ "PAGE", "DEST", "TRIG", "WAVE", "MULT", "SPD", "INTL", "DPTH" },
	};
	for(size_t page = 0; page < pagePanels.size(); ++page)
	{
		verifyLabels(pagePanels[page], pageLabels[page], "page " + std::to_string(page + 1));
		if(!mdJucePlugin::lcdInteraction::classify(pagePanels[page], md::MachineModel::Monomachine, false))
		{
			std::cerr << "page " << page + 1 << ": not recognised as an editable screen\n";
			++errors;
		}
	}

	for(const auto& machine : machines)
	{
		// Manual, MIDI spec: $5b load machine -- track, machine number, 1 = init all data pages.
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex = { 0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x5b, 0x00, static_cast<uint8_t>(machine.id), 0x01, 0xf7 };
		hardware->sendMidi(assign);
		advance(*hardware, md::g_samplerate);
		capture(*hardware, out + "/machine-" + std::to_string(machine.id) + ".pbm");

		const auto panel = hardware->getFrontPanelSnapshot();
		verifyLabels(panel, machine.labels, "machine " + std::to_string(machine.id));

		const auto nameHash = mdJucePlugin::lcdText::hash(panel, mdJucePlugin::lcdText::g_machineName);
		for(const auto& [hash, id] : machineHashes)
		{
			if(hash == nameHash)
			{
				std::cerr << "machine " << machine.id << " name looks identical to machine " << id << '\n';
				++errors;
			}
		}
		machineHashes.emplace_back(nameHash, machine.id);
	}

	std::printf("// Generated by mdmmLcdCapture from Monomachine OS 1.32b screens.\n");
	for(const auto& [hash, label] : labelHashes)
		std::printf("{ 0x%016llxull, \"%s\" },\n", static_cast<unsigned long long>(hash), label.c_str());
	std::printf("// machine names\n");
	for(const auto& [hash, id] : machineHashes)
		std::printf("{ 0x%016llxull, %d },\n", static_cast<unsigned long long>(hash), id);

	std::cerr << "captured to " << out << ", " << labelHashes.size() << " distinct labels, " << errors << " errors\n";
	return errors == 0 ? 0 : 1;
}
