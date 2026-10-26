#include "mdLib/mdRamLabels.h"
#include "mdLib/mdramdiff.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
	using md::ramDiff::Image;
	using md::ramDiff::Report;

	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdRamDiffTest: " << _message << '\n';
		std::exit(1);
	}

	Image makeImage()
	{
		Image image;
		md::ramDiff::prepareImage(image);
		return image;
	}

	void checkNoChanges()
	{
		const auto before = makeImage();
		const auto after = before;
		const auto report = md::ramDiff::diff(before, after);
		expect(report.changedBytes == 0, "identical images reported changes");
		expect(report.rows.empty(), "identical images produced rows");
		const auto text = md::ramDiff::formatHex(report, "idle");
		expect(text.find("no changes") != std::string::npos, "empty diff omitted no-changes line");
	}

	void checkAlignedRows()
	{
		auto before = makeImage();
		auto after = before;
		after.patch.bytes[0x10] = 0x01;
		after.patch.bytes[0x11] = 0x02;
		after.patch.bytes[0x20] = 0xff;
		after.main.bytes[4] = 0xaa;

		const auto report = md::ramDiff::diff(before, after);
		expect(report.changedBytes == 4, "changed byte count is wrong");
		expect(report.rows.size() == 3, "changed bytes were not grouped into 16-byte rows");
		expect(report.rows[0].region == std::string("patch"), "first row region is wrong");
		expect(report.rows[0].address == md::memorymap::g_patchBootstrap.begin + 0x10,
			"first row was not aligned to the changed 16-byte block");
		expect(report.rows[0].before[0] == 0x00 && report.rows[0].after[0] == 0x01,
			"first row does not keep original then updated bytes");
		expect((report.rows[0].changedMask & 0x3) == 0x3, "first row change mask is wrong");
		expect(report.rows[1].address == md::memorymap::g_patchBootstrap.begin + 0x20,
			"second patch row address is wrong");
		expect(report.rows[2].region == std::string("main"), "main-RAM row is missing");
		expect(report.rows[2].address == md::memorymap::g_mainRam.begin,
			"main-RAM row was not aligned to 16 bytes");
		expect(report.rows[2].after[4] == 0xaa, "main-RAM changed byte is in the wrong column");

		const auto text = md::ramDiff::formatHex(report, "Trigger1");
		expect(text.find("RAM diff [Trigger1] 4 bytes in 3 rows") != std::string::npos,
			"summary line is wrong");
		expect(text.find("patch 0x00100010") != std::string::npos, "patch row address missing");
		expect(text.find("[01] [02]") != std::string::npos, "updated changed bytes missing");
		expect(text.find("main 0x00200000") != std::string::npos, "main row address missing");

		const auto rml = md::ramDiff::formatRml(report, "Trigger1");
		expect(rml.find("ramDiffChangedBefore") != std::string::npos, "original highlight class missing");
		expect(rml.find("ramDiffChangedAfter") != std::string::npos, "updated highlight class missing");
	}

	void checkTruncation()
	{
		auto before = makeImage();
		auto after = before;
		for(size_t i = 0; i < 32; ++i)
			after.sram.bytes[i] = static_cast<uint8_t>(i + 1);

		const auto report = md::ramDiff::diff(before, after);
		expect(report.changedBytes == 32, "sram row length is wrong");
		expect(report.rows.size() == 2, "32 sram bytes were not split into two rows");
		const auto text = md::ramDiff::formatHex(report, "long", 1);
		expect(text.find("... 1 more rows") != std::string::npos, "extra rows were not truncated");
	}

	void checkRegionFilter()
	{
		auto before = makeImage();
		auto after = before;
		after.patch.bytes[0] = 1;
		after.main.bytes[0] = 2;
		after.sram.bytes[0] = 3;
		after.loader.bytes[0] = 4;

		const auto all = md::ramDiff::diff(before, after);
		expect(all.rows.size() == 4, "all-region diff dropped a region");
		const auto patchOnly = md::ramDiff::filter(all, md::ramDiff::g_regionPatch);
		expect(patchOnly.rows.size() == 1 && patchOnly.changedBytes == 1,
			"patch filter did not hide the other regions");
		expect(patchOnly.rows[0].kind == md::ramDiff::RegionKind::Patch, "patch filter kept the wrong row");
	}

	void checkSizeMismatch()
	{
		auto before = makeImage();
		auto after = before;
		after.patch.bytes.resize(after.patch.bytes.size() - 1);
		after.patch.bytes[0] = 0x11;
		const auto report = md::ramDiff::diff(before, after);
		expect(report.sizeMismatch, "size mismatch was not reported");
		expect(report.changedBytes == 1, "overlapping byte was not compared");
	}

	void checkRamLabels()
	{
		const auto* trig1 = md::ramLabels::patternFieldAt(0);
		expect(trig1 && std::string(trig1->label) == "pattern triggers 1-32 track 1",
			"track 1 trigger label is wrong");
		const auto* trig2 = md::ramLabels::patternFieldAt(4);
		expect(trig2 && trig2->track == 2, "track 2 trigger field is missing");
		const auto* synth = md::ramLabels::kitFieldAt(0x10);
		expect(synth && std::string(synth->label) == "track 1 synthesis parameters 1-8",
			"track 1 synthesis label is wrong");
		const auto* effects = md::ramLabels::kitFieldAt(0x18);
		expect(effects && std::string(effects->label) == "track 1 effects parameters 1-8",
			"track 1 effects label is wrong");
		const auto* route16 = md::ramLabels::kitFieldAt(0x188);
		expect(route16 && route16->track == 16 && route16->kind == md::ramLabels::Kind::Routing,
			"track 16 routing field is missing");
		const auto* lfo1 = md::ramLabels::kitFieldAt(0x1e0);
		expect(lfo1 && lfo1->kind == md::ramLabels::Kind::Lfo && lfo1->track == 1,
			"track 1 LFO field is missing");

		md::ramLabels::Map map;
		const auto base = md::memorymap::g_patchBootstrap.begin;
		const auto trigRow = md::ramLabels::annotateRow(map, md::ramDiff::RegionKind::Patch,
			base, md::ramLabels::Annotate::Pattern, base);
		expect(trigRow && trigRow->text.find("tracks 1-4") != std::string::npos,
			"pattern row did not group triggers for tracks 1-4");
		const auto kitRow = md::ramLabels::annotateRow(map, md::ramDiff::RegionKind::Patch,
			base + 0x10, md::ramLabels::Annotate::Kit, base);
		expect(kitRow && kitRow->text.find("synth") != std::string::npos
			&& kitRow->text.find("fx") != std::string::npos,
			"kit row did not mark synthesis and effects");
		expect(!md::ramLabels::annotateRow(map, md::ramDiff::RegionKind::Sram,
			md::memorymap::g_internalSram.begin, md::ramLabels::Annotate::Pattern,
			md::memorymap::g_internalSram.begin),
			"sram must not be labelled as a pattern record");
		expect(!md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch, base,
			md::ramLabels::Annotate::Off, base),
			"labels-off still returned a hit");

		map = md::ramLabels::probedMap();
		const auto probed = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedPatternBase + 3, md::ramLabels::Annotate::Pattern);
		expect(probed && probed->field && probed->field->track == 1
			&& probed->field->kind == md::ramLabels::Kind::Trig
			&& probed->text.find("A01") != std::string::npos,
			"probed pattern base does not label A01 track 1 triggers");
		const auto a02 = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedPatternBase + md::ramLabels::g_probedPatternStride + 3,
			md::ramLabels::Annotate::Pattern);
		expect(a02 && a02->recordIndex == 1 && a02->text.find("A02") != std::string::npos,
			"pattern stride does not label A02");
		const auto b01 = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedPatternBase + 16 * md::ramLabels::g_probedPatternStride + 3,
			md::ramLabels::Annotate::Pattern);
		expect(b01 && b01->recordIndex == 16 && b01->text.find("B01") != std::string::npos,
			"pattern stride does not label B01");
		const auto probedRow = md::ramLabels::annotateRow(map, md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedPatternBase, md::ramLabels::Annotate::Pattern, UINT32_MAX);
		expect(probedRow && probedRow->text.find("A01") != std::string::npos
			&& probedRow->text.find("tracks 1-4") != std::string::npos,
			"probed pattern row did not group A01 tracks 1-4");
		const auto kitA = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			0x00100022, md::ramLabels::Annotate::Kit);
		expect(kitA && kitA->field && kitA->field->kind == md::ramLabels::Kind::Synth
			&& kitA->field->track == 1,
			"probed kit base does not label track 1 synthesis A");
		const auto kitFx = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			0x0010002a, md::ramLabels::Annotate::Kit);
		expect(kitFx && kitFx->field && kitFx->field->kind == md::ramLabels::Kind::Effects
			&& kitFx->field->track == 1,
			"probed kit base does not label track 1 effects A");
		const auto current = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedCurrentPattern, md::ramLabels::Annotate::All);
		expect(current && current->field
			&& current->field->kind == md::ramLabels::Kind::CurrentPattern,
			"current pattern pointer is not labelled");
		const auto song = md::ramLabels::hit(map, md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedSongBase + 2, md::ramLabels::Annotate::Song);
		expect(song && song->field && song->field->kind == md::ramLabels::Kind::SongName
			&& song->text.find("song 01") != std::string::npos,
			"song 01 name is not labelled");

		expect(md::ramDiff::ramRangeValid(md::ramDiff::RegionKind::Patch,
			md::ramLabels::g_probedPatternBase, 4),
			"pattern A01 trig word should be a valid range");
		expect(!md::ramDiff::ramRangeValid(md::ramDiff::RegionKind::Patch,
			md::memorymap::g_patchBootstrap.begin, md::ramDiff::g_rangeIoMaxBytes + 1),
			"range cap was not enforced");

		const auto trig = md::ramLabels::resolveSegment(map, "pattern:A01:trigs:1");
		expect(trig && trig->address == md::ramLabels::g_probedPatternBase
			&& trig->size == 4,
			"pattern:A01:trigs:1 did not resolve to A01 track 1");
		const auto a02slot = md::ramLabels::resolveSegment(map, "pattern:A02");
		expect(a02slot && a02slot->address == md::ramLabels::g_probedPatternBase
			+ md::ramLabels::g_probedPatternStride,
			"pattern:A02 did not use the probed stride");
		const auto kitSynth = md::ramLabels::resolveSegment(map, "kit:current:synth:1");
		expect(kitSynth && kitSynth->address == 0x00100022 && kitSynth->size == 8,
			"kit:current:synth:1 did not resolve to T1 synth A-H");
		const auto listed = md::ramLabels::listSegments(map, "pattern");
		expect(listed.size() == 128 && listed.front().name == "pattern:A01"
			&& listed.back().name == "pattern:H16",
			"pattern catalogue is not A01-H16");
	}
}

int main()
{
	checkNoChanges();
	checkAlignedRows();
	checkTruncation();
	checkRegionFilter();
	checkSizeMismatch();
	checkRamLabels();
	return 0;
}
