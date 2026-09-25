#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmidiprotocol.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdRamLabels.h"
#include "mdLib/mdromloader.h"

#include "baseLib/filesystem.h"
#include "sysexPanelDriver.h"
#include "synthLib/midiTypes.h"
#include "synthLib/os.h"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	struct Snap
	{
		std::vector<uint8_t> patch;
		std::vector<uint8_t> main;
	};

	struct Change
	{
		md::ramDiff::RegionKind kind = md::ramDiff::RegionKind::Patch;
		uint32_t address = 0;
		uint8_t before = 0;
		uint8_t after = 0;
	};

	void sendSysex(md::Hardware& _hardware, const md::midiProtocol::SysexBody& _body)
	{
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
		event.sysex.push_back(0xf0);
		event.sysex.insert(event.sysex.end(), _body.begin(), _body.end());
		event.sysex.push_back(0xf7);
		md::test::require(_hardware.sendMidi(event), "sysex rejected");
		md::test::advanceFrames(_hardware, md::g_samplerate / 4);
	}

	Snap snapshot(md::Hardware& _hardware)
	{
		Snap snap;
		_hardware.copyWorkingRamRegion(md::ramDiff::RegionKind::Patch, snap.patch);
		_hardware.copyWorkingRamRegion(md::ramDiff::RegionKind::Main, snap.main);
		return snap;
	}

	void collect(std::vector<Change>& _out, const md::ramDiff::RegionKind _kind,
		const std::vector<uint8_t>& _before, const std::vector<uint8_t>& _after)
	{
		const auto base = md::ramDiff::regionRange(_kind).begin;
		const auto n = std::min(_before.size(), _after.size());
		for(size_t i = 0; i < n; ++i)
		{
			if(_before[i] == _after[i])
				continue;
			_out.push_back({_kind, base + static_cast<uint32_t>(i), _before[i], _after[i]});
		}
	}

	void printOne(const Change& _change, const md::ramLabels::Map& _map)
	{
		std::printf("  %s 0x%08x  %02x -> %02x  xor %02x",
			md::ramDiff::regionName(_change.kind), _change.address, _change.before,
			_change.after, static_cast<unsigned>(_change.before ^ _change.after));
		if(const auto found = md::ramLabels::hit(_map, _change.kind, _change.address,
			md::ramLabels::Record::Pattern, md::ramLabels::g_probedPatternBase))
			std::printf("  [%s]", found->text.c_str());
		else if(const auto found = md::ramLabels::hit(_map, _change.kind, _change.address,
			md::ramLabels::Record::Kit, md::ramLabels::g_probedKitBase))
			std::printf("  [%s]", found->text.c_str());
		std::printf("\n");
	}

	void printChanges(const std::string& _label, const std::vector<Change>& _changes)
	{
		std::vector<Change> patch;
		std::vector<Change> main;
		for(const auto& change : _changes)
			(change.kind == md::ramDiff::RegionKind::Patch ? patch : main).push_back(change);
		std::cout << _label << "  (patch " << patch.size() << "  main " << main.size() << ")\n";
		const auto map = md::ramLabels::probedMap();
		constexpr size_t kMax = 24;
		const auto dump = [&](const std::vector<Change>& _list)
		{
			for(size_t i = 0; i < _list.size() && i < kMax; ++i)
				printOne(_list[i], map);
			if(_list.size() > kMax)
				std::cout << "  ... " << (_list.size() - kMax) << " more\n";
		};
		if(!patch.empty())
			dump(patch);
		if(patch.empty() && !main.empty())
			dump(main);
		else if(!main.empty())
			std::cout << "  (main " << main.size() << " bytes omitted)\n";
		if(_changes.empty())
			std::cout << "  no patch/main changes\n";
	}

	void suggestBase(const char* const _name, const uint32_t _fieldOffset,
		const std::vector<Change>& _changes)
	{
		std::vector<Change> patch;
		for(const auto& change : _changes)
		{
			if(change.kind == md::ramDiff::RegionKind::Patch)
				patch.push_back(change);
		}
		const auto& used = patch.empty() ? _changes : patch;
		if(used.empty())
		{
			std::cout << "  " << _name << ": no changes\n";
			return;
		}
		uint32_t lo = UINT32_MAX;
		uint32_t hi = 0;
		for(const auto& change : used)
		{
			lo = std::min(lo, change.address);
			hi = std::max(hi, change.address);
		}
		const auto suggested = lo >= _fieldOffset ? lo - _fieldOffset : lo;
		std::printf("  %s: first 0x%08x last 0x%08x  suggested base 0x%08x (field +0x%x, %s)\n",
			_name, lo, hi, suggested, _fieldOffset, patch.empty() ? "main" : "patch");
	}

	std::string pluginHome()
	{
		const auto* const dataRoot = std::getenv("GEARMULATOR_DATA_ROOT");
		const auto root = dataRoot != nullptr && *dataRoot != '\0'
			? baseLib::filesystem::validatePath(dataRoot)
			: baseLib::filesystem::getSpecialFolderPath(
				baseLib::filesystem::SpecialFolderType::UserDocuments);
		return baseLib::filesystem::validatePath(root + "Gearmulator Preview/Machinedrum/");
	}
}

int main()
{
	try
	{
		const auto home = pluginHome();
		synthLib::RomLoader::addSearchPath(home + "roms/");
		synthLib::RomLoader::addSearchPath(synthLib::getModulePath(true));
		synthLib::RomLoader::addSearchPath(synthLib::getModulePath(false));

		const auto rom = md::RomLoader::findROM(md::MachineModel::Machinedrum);
		if(!rom.isValid())
		{
			std::cout << "No Machinedrum OS 1.63 .bin in " << home << "roms/\n";
			return 77;
		}

		synthLib::DeviceCreateParams params;
		params.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
		params.homePath = home;
		auto device = std::make_unique<md::Device>(params);
		md::test::require(device->isValid(), "plugin-style Device rejected the ROM");
		auto& hardware = device->getHardware();
		std::cout << "firmware " << rom.getFilename() << '\n';
		std::cout << "home " << home << '\n';
		std::cout << "factory cache "
			<< (hardware.isFactoryFlashCacheReady() ? "loaded" : "absent (first-run init)")
			<< std::endl;
		std::cout << "booting..." << std::endl;
		md::test::advanceFrames(hardware, md::g_samplerate * 25);
		md::test::require(hardware.isFirmwareMidiReady(), "boot incomplete");
		std::cout << "boot ready" << std::endl;

		if(!hardware.getFrontPanelSnapshot().getModeLed(md::FrontPanel::ModeLed::Extended))
			md::test::panelTap(hardware, md::PanelControl::ClassicExtended);

		const auto md = md::MachineModel::Machinedrum;
		sendSysex(hardware, md::midiProtocol::selectPattern(md, 0));
		sendSysex(hardware, md::midiProtocol::selectTrack(0));
		md::test::panelTap(hardware, md::PanelControl::Record);

		const auto run = [&](const std::string& label, auto&& edit)
		{
			std::vector<Change> changes;
			const auto before = snapshot(hardware);
			edit();
			md::test::advanceFrames(hardware, md::g_samplerate / 5);
			const auto after = snapshot(hardware);
			collect(changes, md::ramDiff::RegionKind::Patch, before.patch, after.patch);
			collect(changes, md::ramDiff::RegionKind::Main, before.main, after.main);
			printChanges(label, changes);
			return changes;
		};

		const auto firstPatch = [](const std::vector<Change>& _changes) -> uint32_t
		{
			for(const auto& change : _changes)
			{
				if(change.kind == md::ramDiff::RegionKind::Patch)
					return change.address;
			}
			return 0;
		};

		run("trig A01 T1S1", [&] { md::test::panelTap(hardware, md::PanelControl::Trigger1); });
		const auto a01 = firstPatch(run("trig A01 T1S2", [&]
		{
			md::test::panelTap(hardware, md::PanelControl::Trigger2);
		}));
		sendSysex(hardware, md::midiProtocol::selectPattern(md, 1));
		const auto a02 = firstPatch(run("trig A02 T1S1", [&]
		{
			md::test::panelTap(hardware, md::PanelControl::Trigger1);
		}));
		sendSysex(hardware, md::midiProtocol::selectPattern(md, 16));
		const auto b01 = firstPatch(run("trig B01 T1S1", [&]
		{
			md::test::panelTap(hardware, md::PanelControl::Trigger1);
		}));
		sendSysex(hardware, md::midiProtocol::selectPattern(md, 127));
		const auto h16 = firstPatch(run("trig H16 T1S1", [&]
		{
			md::test::panelTap(hardware, md::PanelControl::Trigger1);
		}));

		const auto tweakA = [&]
		{
			md::test::require(hardware.trySendPanelEvent(
				*md::panelEncoderCommand(md, md::PanelEncoder::DataEntryA), 1),
				"encoder rejected");
			md::test::advanceFrames(hardware, md::g_samplerate / 4);
		};

		sendSysex(hardware, md::midiProtocol::selectPattern(md, 0));
		sendSysex(hardware, md::midiProtocol::selectTrack(0));
		md::test::panelTap(hardware, md::PanelControl::Record);
		md::test::panelTap(hardware, md::PanelControl::Kit);
		md::test::panelTap(hardware, md::PanelControl::SynthesisEffectsRouting);

		sendSysex(hardware, md::midiProtocol::selectKit(0));
		const auto kit0 = firstPatch(run("kit 01 synth A", tweakA));
		sendSysex(hardware, md::midiProtocol::selectKit(1));
		const auto kit1 = firstPatch(run("kit 02 synth A", tweakA));
		sendSysex(hardware, md::midiProtocol::selectKit(2));
		const auto kit2 = firstPatch(run("kit 03 synth A", tweakA));
		sendSysex(hardware, md::midiProtocol::selectKit(63));
		const auto kit64 = firstPatch(run("kit 64 synth A", tweakA));

		const auto huntPointer = [&](const char* const name, const uint8_t parameter,
			const int a, const int b, const int c)
		{
			sendSysex(hardware, md::midiProtocol::setStatus(md, parameter, static_cast<uint8_t>(a)));
			md::test::advanceFrames(hardware, md::g_samplerate / 4);
			auto before = snapshot(hardware);
			sendSysex(hardware, md::midiProtocol::setStatus(md, parameter, static_cast<uint8_t>(b)));
			md::test::advanceFrames(hardware, md::g_samplerate / 4);
			auto mid = snapshot(hardware);
			std::vector<Change> first;
			collect(first, md::ramDiff::RegionKind::Patch, before.patch, mid.patch);
			collect(first, md::ramDiff::RegionKind::Main, before.main, mid.main);
			sendSysex(hardware, md::midiProtocol::setStatus(md, parameter, static_cast<uint8_t>(c)));
			md::test::advanceFrames(hardware, md::g_samplerate / 4);
			auto after = snapshot(hardware);
			std::vector<Change> second;
			collect(second, md::ramDiff::RegionKind::Patch, mid.patch, after.patch);
			collect(second, md::ramDiff::RegionKind::Main, mid.main, after.main);
			std::cout << "current " << name << " pointer (" << a << " -> " << b << " -> " << c << ")\n";
			size_t hits = 0;
			for(const auto& one : first)
			{
				if(one.before != static_cast<uint8_t>(a) || one.after != static_cast<uint8_t>(b))
					continue;
				for(const auto& two : second)
				{
					if(two.address != one.address || two.kind != one.kind)
						continue;
					if(two.before != static_cast<uint8_t>(b) || two.after != static_cast<uint8_t>(c))
						continue;
					std::printf("  %s 0x%08x  %u -> %u -> %u\n",
						md::ramDiff::regionName(one.kind), one.address, a, b, c);
					++hits;
				}
			}
			if(!hits)
				std::cout << "  no unique " << name << " pointer\n";
		};

		huntPointer("pattern", 0x04, 0, 3, 7);
		huntPointer("kit", 0x02, 0, 3, 7);
		huntPointer("song", 0x08, 0, 3, 7);
		huntPointer("global", 0x01, 0, 3, 7);

		sendSysex(hardware, md::midiProtocol::selectSong(0));
		md::test::advanceFrames(hardware, md::g_samplerate / 4);
		run("select song 02", [&]
		{
			sendSysex(hardware, md::midiProtocol::selectSong(1));
		});

		std::cout << "strides\n";
		std::printf("  pattern A01 0x%08x  A02 0x%08x  B01 0x%08x  H16 0x%08x\n",
			a01, a02, b01, h16);
		if(a01 && a02 && a02 > a01)
			std::printf("  pattern stride 0x%x  (A02-A01)\n", a02 - a01);
		if(a01 && b01 && b01 > a01)
			std::printf("  pattern bank stride 0x%x  (B01-A01)/16 = 0x%x\n",
				b01 - a01, (b01 - a01) / 16);
		std::printf("  kit 01 0x%08x  kit 02 0x%08x  kit 03 0x%08x  kit 64 0x%08x\n",
			kit0, kit1, kit2, kit64);
		if(kit0 && kit1 && kit1 > kit0)
			std::printf("  kit stride 0x%x  (02-01)\n", kit1 - kit0);
		if(kit1 && kit2 && kit2 > kit1)
			std::printf("  kit stride 0x%x  (03-02)\n", kit2 - kit1);
		(void)h16;
		(void)kit64;
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
