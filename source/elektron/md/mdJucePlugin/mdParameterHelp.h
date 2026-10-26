#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mdJucePlugin::parameterHelp
{
	// Types shared by the Monomachine and Machinedrum help tables.

	// A text drawn on the LCD, identified by the hash of its pixels (see mdLcdText.h).
	struct LabelHash
	{
		uint64_t hash;
		const char* label;
	};

	// A setting's value as the LCD prints it, with what that choice means.
	struct ValueHash
	{
		uint64_t hash;
		const char* text;
		const char* meaning;
	};

	// The row of _table whose hash is _hash, or nullptr.
	template<typename T, size_t N>
	const T* findByHash(const T (&_table)[N], const uint64_t _hash)
	{
		for(const auto& row : _table)
			if(row.hash == _hash)
				return &row;
		return nullptr;
	}

	// The LCD text of _hash in _table, or nullptr.
	template<size_t N>
	const char* labelForHash(const LabelHash (&_table)[N], const uint64_t _hash)
	{
		const auto* const row = findByHash(_table, _hash);
		return row ? row->label : nullptr;
	}

	// Plain-language help for the Monomachine DATA ENTRY knobs, shown as hover tooltips.
	// Abbreviations are the labels the firmware draws on the LCD. Knob order matches
	// parameterDescriptions_mm.json (A-D top row, E-H bottom row).
	struct Entry
	{
		const char* abbreviation;
		const char* name;
		const char* description;
	};

	using Page = std::array<Entry, 8>;

	// Index 0 (SYNTHESIS) is machine specific and has no fixed entries.
	inline constexpr Page g_monomachineAmplification
	{{
		{ "ATK",  "Attack",      "How long the amp envelope takes to rise to full level." },
		{ "HOLD", "Hold",        "How long the note stays at full level before it decays. Long holds work like a sustained note." },
		{ "DEC",  "Decay",       "How long the sound takes to fade out once the hold time has passed." },
		{ "REL",  "Release",     "How quickly the sound fades after a NOTE OFF." },
		{ "DIST", "Distortion",  "Overdrive built into the filter. Also sets the headroom of the filter and EQ." },
		{ "VOL",  "Volume",      "The track's volume. Unlike LEVEL, this one can be locked and modulated by LFOs." },
		{ "PAN",  "Pan",         "Position in the stereo field: -64 is hard left, +63 hard right." },
		{ "PORT", "Portamento",  "Glide time between one note's pitch and the next." },
	}};

	inline constexpr Page g_monomachineFilter
	{{
		{ "BASE", "Filter base",            "The lower edge of the filter band. With WDTH at maximum it acts as a high-pass cutoff." },
		{ "WDTH", "Filter width",           "The gap between the high-pass and low-pass cutoffs. With BASE at minimum it acts as a low-pass cutoff." },
		{ "HPQ",  "High-pass resonance",    "How much the level is boosted around the high-pass cutoff." },
		{ "LPQ",  "Low-pass resonance",     "How much the level is boosted around the low-pass cutoff." },
		{ "ATK",  "Filter envelope attack", "Attack time of the filter envelope." },
		{ "DEC",  "Filter envelope decay",  "Decay time of the filter envelope." },
		{ "BOFS", "Base envelope offset",   "How far the filter envelope pushes BASE." },
		{ "WOFS", "Width envelope offset",  "How far the filter envelope pushes WDTH." },
	}};

	inline constexpr Page g_monomachineEffects
	{{
		{ "EQF",  "EQ frequency",          "The frequency the EQ boosts or cuts." },
		{ "EQG",  "EQ gain",               "Boost (positive) or cut (negative) around EQF, up to 36 dB." },
		{ "SRR",  "Sample-rate reduction", "Lo-fi downsampling, down to as low as 2.8 kHz." },
		{ "DTIM", "Delay time",            "Delay length in 256th notes relative to tempo. 64 is one beat." },
		{ "DSND", "Delay send",            "How much of the track goes into the delay. Nothing echoes until this is above zero." },
		{ "DFB",  "Delay feedback",        "How much of the delay's output is fed back in. High values give long or endless echoes." },
		{ "DBAS", "Delay filter base",     "High-pass filtering inside the delay's feedback loop." },
		{ "DWID", "Delay filter width",    "Low-pass filtering inside the delay's feedback loop, relative to DBAS." },
	}};

	inline constexpr Page g_monomachineLfo
	{{
		{ "PAGE", "Target page",  "Which DATA page this LFO modulates." },
		{ "DEST", "Destination",  "Which parameter on the page chosen by PAGE is modulated." },
		{ "TRIG", "Trig mode",    "How note trigs restart, hold or gate the LFO." },
		{ "WAVE", "Waveform",     "The LFO shape, from eleven waveforms." },
		{ "MULT", "Multiplier",   "Multiplies SPD. Each step up halves the cycle time." },
		{ "SPD",  "Speed",        "Base rate, locked to tempo. 16, 32, 64 or 127 land on straight beats." },
		{ "INTL", "Interlace",    "Alternates the LFO with silence at this rate. 0 turns it off." },
		{ "DPTH", "Depth",        "How strongly the LFO moves the target parameter." },
	}};

	// _page follows the DATA PAGE LEDs: 0 SYNTHESIS, 1 AMP, 2 FILTER, 3 EFFECTS, 4-6 LFO 1-3.
	// SYNTHESIS depends on the machine, so it has no fixed page.
	inline constexpr const Page* monomachinePage(const int _page)
	{
		switch(_page)
		{
		case 1: return &g_monomachineAmplification;
		case 2: return &g_monomachineFilter;
		case 3: return &g_monomachineEffects;
		case 4: case 5: case 6: return &g_monomachineLfo;
		default: return nullptr;
		}
	}

	inline constexpr const Entry* monomachineEntry(const int _page, const size_t _encoder)
	{
		const auto* const page = monomachinePage(_page);
		return page && _encoder < page->size() ? &(*page)[_encoder] : nullptr;
	}

	// The entry of _page whose LCD label is _label, or nullptr.
	inline const Entry* entryForLabel(const Page& _page, const char* const _label)
	{
		for(const auto& entry : _page)
			if(std::strcmp(entry.abbreviation, _label) == 0)
				return &entry;
		return nullptr;
	}

	inline constexpr const char* g_monomachinePageNames[] =
		{ "Synthesis", "Amplification", "Filter", "Effects", "LFO 1", "LFO 2", "LFO 3" };
}
