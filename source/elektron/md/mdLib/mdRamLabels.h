#pragma once

#include "mdramdiff.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace md::ramLabels
{
	// Unpacked Machinedrum kit/pattern layouts from Elektron "System Exclusive
	// description" v0.9 (OS 1.53 dump shape, still used by OS 1.63). Offsets are
	// relative to one record in RAM, not SysEx positions: 7-bit encoded blobs in
	// the PDF unpack to these sizes. Absolute bases are filled by
	// mdPatchRamMapFirmwareTest when firmware is available.
	enum class Record : uint8_t
	{
		Pattern,
		Kit,
		Song,
		Global,
		Current,
	};

	enum class Annotate : uint8_t
	{
		Off,
		All,
		Pattern,
		Kit,
		Song,
		Global,
	};

	enum class Kind : uint8_t
	{
		None,
		Trig,
		LockMask,
		Accent,
		Slide,
		Swing,
		SwingAmount,
		PatternHeader,
		ParamLocks,
		KitName,
		Synth,
		Effects,
		Routing,
		Level,
		Model,
		Lfo,
		Reverb,
		Delay,
		Eq,
		Dynamics,
		TrigGroup,
		MuteGroup,
		SongIndex,
		SongName,
		SongRows,
		GlobalSlot,
		GlobalData,
		CurrentPattern,
		CurrentKit,
		CurrentSong,
		CurrentGlobal,
	};

	struct Field
	{
		Record record = Record::Pattern;
		uint32_t offset = 0;
		uint32_t size = 1;
		const char* label = "";
		const char* shortLabel = "";
		uint32_t color = 0xff6aa84f;
		Kind kind = Kind::None;
		uint8_t track = 0;
	};

	// OS 1.63 patch RAM, mdPatchRamMapFirmwareTest (plugin-style boot).
	// Patterns are stored in place: A01 T1 LSB 0x001272c3, A02 0x00127b59,
	// B01 0x0012fc23. Stride 0x896 = trigs/locks/accent (0x90) + 6 header bytes
	// + 64 lock rows of 32. Kits load into a working copy at 0x00100000
	// (SET STATUS kit does not swap the buffer; LOAD KIT does).
	inline constexpr uint32_t g_probedPatternBase = 0x001272c0;
	inline constexpr uint32_t g_probedPatternStride = 0x896;
	inline constexpr uint32_t g_probedPatternCount = 128;
	inline constexpr uint32_t g_probedKitBase = 0x00100012;
	inline constexpr uint32_t g_probedKitStride = 0;
	inline constexpr uint32_t g_probedKitCount = 1;
	inline constexpr uint32_t g_probedSongBase = 0x001126ac;
	inline constexpr uint32_t g_probedSongStride = 0xa10;
	inline constexpr uint32_t g_probedSongCount = 32;
	inline constexpr uint32_t g_probedGlobalBase = 0x001120ca;
	inline constexpr uint32_t g_probedGlobalStride = 0xbc;
	inline constexpr uint32_t g_probedGlobalCount = 8;
	inline constexpr uint32_t g_probedCurrentPattern = 0x001272be;
	inline constexpr uint32_t g_probedCurrentKit = 0x00100008;
	inline constexpr uint32_t g_probedCurrentSong = 0x001126ac;
	inline constexpr uint32_t g_probedCurrentGlobal = 0x001120ca;

	struct Map
	{
		uint32_t patternBase = UINT32_MAX;
		uint32_t patternStride = 0;
		uint32_t patternCount = 128;
		uint32_t kitBase = UINT32_MAX;
		uint32_t kitStride = 0;
		uint32_t kitCount = 64;
		uint32_t songBase = UINT32_MAX;
		uint32_t songStride = 0;
		uint32_t songCount = 32;
		uint32_t globalBase = UINT32_MAX;
		uint32_t globalStride = 0;
		uint32_t globalCount = 8;
		uint32_t currentPattern = UINT32_MAX;
		uint32_t currentKit = UINT32_MAX;
		uint32_t currentSong = UINT32_MAX;
		uint32_t currentGlobal = UINT32_MAX;
	};

	inline Map probedMap()
	{
		Map map;
		map.patternBase = g_probedPatternBase;
		map.patternStride = g_probedPatternStride;
		map.patternCount = g_probedPatternCount;
		map.kitBase = g_probedKitBase;
		map.kitStride = g_probedKitStride;
		map.kitCount = g_probedKitCount;
		map.songBase = g_probedSongBase;
		map.songStride = g_probedSongStride;
		map.songCount = g_probedSongCount;
		map.globalBase = g_probedGlobalBase;
		map.globalStride = g_probedGlobalStride;
		map.globalCount = g_probedGlobalCount;
		map.currentPattern = g_probedCurrentPattern;
		map.currentKit = g_probedCurrentKit;
		map.currentSong = g_probedCurrentSong;
		map.currentGlobal = g_probedCurrentGlobal;
		return map;
	}

	inline std::string patternSlotName(const uint32_t _index)
	{
		char text[8];
		std::snprintf(text, sizeof(text), "%c%02u",
			static_cast<char>('A' + _index / 16), (_index % 16) + 1);
		return text;
	}

	inline std::string kitSlotName(const uint32_t _index)
	{
		return "kit " + std::to_string(_index + 1);
	}

	inline std::string songSlotName(const uint32_t _index)
	{
		char text[12];
		std::snprintf(text, sizeof(text), "song %02u", _index + 1);
		return text;
	}

	inline std::string globalSlotName(const uint32_t _index)
	{
		return "global " + std::to_string(_index + 1);
	}

	inline constexpr uint32_t g_trigColor = 0xff6aa84f;
	inline constexpr uint32_t g_lockColor = 0xffe69138;
	inline constexpr uint32_t g_accentColor = 0xff3d85c6;
	inline constexpr uint32_t g_kitNameColor = 0xffb4a7d6;
	inline constexpr uint32_t g_synthColor = 0xff6fa8dc;
	inline constexpr uint32_t g_effectsColor = 0xff76a5af;
	inline constexpr uint32_t g_routingColor = 0xfff1c232;
	inline constexpr uint32_t g_levelColor = 0xffa64d79;
	inline constexpr uint32_t g_modelColor = 0xff8e7cc3;
	inline constexpr uint32_t g_lfoColor = 0xffc27ba0;
	inline constexpr uint32_t g_fxColor = 0xffd5a6bd;
	inline constexpr uint32_t g_groupColor = 0xff93c47d;

#define MD_RAM_TRIG(n, off) \
	{Record::Pattern, off, 4, "pattern triggers 1-32 track " #n, "trigs T" #n " 1-32", g_trigColor, Kind::Trig, n}
#define MD_RAM_LOCK(n, off) \
	{Record::Pattern, off, 4, "pattern lock params 1-24 track " #n, "locks T" #n, g_lockColor, Kind::LockMask, n}
#define MD_RAM_KIT_TRACK(n, off) \
	{Record::Kit, (off) + 0x00, 8, "track " #n " synthesis parameters 1-8", "T" #n " synth 1-8", g_synthColor, Kind::Synth, n}, \
	{Record::Kit, (off) + 0x08, 8, "track " #n " effects parameters 1-8", "T" #n " fx 1-8", g_effectsColor, Kind::Effects, n}, \
	{Record::Kit, (off) + 0x10, 8, "track " #n " routing parameters 1-8", "T" #n " route 1-8", g_routingColor, Kind::Routing, n}
#define MD_RAM_LFO(n, off) \
	{Record::Kit, off, 36, "track " #n " LFO", "T" #n " LFO", g_lfoColor, Kind::Lfo, n}

	inline constexpr Field g_patternFields[] = {
		MD_RAM_TRIG(1, 0x00), MD_RAM_TRIG(2, 0x04), MD_RAM_TRIG(3, 0x08), MD_RAM_TRIG(4, 0x0c),
		MD_RAM_TRIG(5, 0x10), MD_RAM_TRIG(6, 0x14), MD_RAM_TRIG(7, 0x18), MD_RAM_TRIG(8, 0x1c),
		MD_RAM_TRIG(9, 0x20), MD_RAM_TRIG(10, 0x24), MD_RAM_TRIG(11, 0x28), MD_RAM_TRIG(12, 0x2c),
		MD_RAM_TRIG(13, 0x30), MD_RAM_TRIG(14, 0x34), MD_RAM_TRIG(15, 0x38), MD_RAM_TRIG(16, 0x3c),
		MD_RAM_LOCK(1, 0x40), MD_RAM_LOCK(2, 0x44), MD_RAM_LOCK(3, 0x48), MD_RAM_LOCK(4, 0x4c),
		MD_RAM_LOCK(5, 0x50), MD_RAM_LOCK(6, 0x54), MD_RAM_LOCK(7, 0x58), MD_RAM_LOCK(8, 0x5c),
		MD_RAM_LOCK(9, 0x60), MD_RAM_LOCK(10, 0x64), MD_RAM_LOCK(11, 0x68), MD_RAM_LOCK(12, 0x6c),
		MD_RAM_LOCK(13, 0x70), MD_RAM_LOCK(14, 0x74), MD_RAM_LOCK(15, 0x78), MD_RAM_LOCK(16, 0x7c),
		{Record::Pattern, 0x80, 4, "pattern accent steps 1-32", "accent 1-32", g_accentColor, Kind::Accent, 0},
		{Record::Pattern, 0x84, 4, "pattern slide steps 1-32", "slide 1-32", g_accentColor, Kind::Slide, 0},
		{Record::Pattern, 0x88, 4, "pattern swing steps 1-32", "swing 1-32", g_accentColor, Kind::Swing, 0},
		{Record::Pattern, 0x8c, 4, "pattern swing amount", "swing amount", g_accentColor, Kind::SwingAmount, 0},
		{Record::Pattern, 0x90, 1, "pattern accent amount", "accent amt", g_accentColor, Kind::PatternHeader, 0},
		{Record::Pattern, 0x91, 1, "pattern length 1-64", "length", g_accentColor, Kind::PatternHeader, 0},
		{Record::Pattern, 0x92, 1, "pattern tempo multiplier", "tempo mul", g_accentColor, Kind::PatternHeader, 0},
		{Record::Pattern, 0x93, 1, "pattern scale 16/32/48/64", "scale", g_accentColor, Kind::PatternHeader, 0},
		{Record::Pattern, 0x94, 1, "pattern kit number", "kit#", g_accentColor, Kind::PatternHeader, 0},
		{Record::Pattern, 0x95, 1, "pattern lock row count", "lock rows", g_lockColor, Kind::PatternHeader, 0},
		{Record::Pattern, 0x96, 0x800, "pattern parameter locks", "locks", g_lockColor, Kind::ParamLocks, 0},
	};

	// Kit payload offsets from the 16-byte name. Track parameters are 24 bytes
	// each (synth 1-8, effects 1-8, routing 1-8) as on the panel DATA ENTRY row.
	inline constexpr Field g_kitFields[] = {
		{Record::Kit, 0x00, 16, "kit name", "kit name", g_kitNameColor, Kind::KitName, 0},
		MD_RAM_KIT_TRACK(1, 0x10),
		MD_RAM_KIT_TRACK(2, 0x28),
		MD_RAM_KIT_TRACK(3, 0x40),
		MD_RAM_KIT_TRACK(4, 0x58),
		MD_RAM_KIT_TRACK(5, 0x70),
		MD_RAM_KIT_TRACK(6, 0x88),
		MD_RAM_KIT_TRACK(7, 0xa0),
		MD_RAM_KIT_TRACK(8, 0xb8),
		MD_RAM_KIT_TRACK(9, 0xd0),
		MD_RAM_KIT_TRACK(10, 0xe8),
		MD_RAM_KIT_TRACK(11, 0x100),
		MD_RAM_KIT_TRACK(12, 0x118),
		MD_RAM_KIT_TRACK(13, 0x130),
		MD_RAM_KIT_TRACK(14, 0x148),
		MD_RAM_KIT_TRACK(15, 0x160),
		MD_RAM_KIT_TRACK(16, 0x178),
		{Record::Kit, 0x190, 16, "kit track levels 1-16", "levels 1-16", g_levelColor, Kind::Level, 0},
		{Record::Kit, 0x1a0, 64, "kit drum models tracks 1-16", "models 1-16", g_modelColor, Kind::Model, 0},
		MD_RAM_LFO(1, 0x1e0), MD_RAM_LFO(2, 0x204), MD_RAM_LFO(3, 0x228), MD_RAM_LFO(4, 0x24c),
		MD_RAM_LFO(5, 0x270), MD_RAM_LFO(6, 0x294), MD_RAM_LFO(7, 0x2b8), MD_RAM_LFO(8, 0x2dc),
		MD_RAM_LFO(9, 0x300), MD_RAM_LFO(10, 0x324), MD_RAM_LFO(11, 0x348), MD_RAM_LFO(12, 0x36c),
		MD_RAM_LFO(13, 0x390), MD_RAM_LFO(14, 0x3b4), MD_RAM_LFO(15, 0x3d8), MD_RAM_LFO(16, 0x3fc),
		{Record::Kit, 0x420, 8, "kit reverb parameters 1-8", "reverb 1-8", g_fxColor, Kind::Reverb, 0},
		{Record::Kit, 0x428, 8, "kit delay parameters 1-8", "delay 1-8", g_fxColor, Kind::Delay, 0},
		{Record::Kit, 0x430, 8, "kit EQ parameters 1-8", "EQ 1-8", g_fxColor, Kind::Eq, 0},
		{Record::Kit, 0x438, 8, "kit dynamics parameters 1-8", "dynamics 1-8", g_fxColor, Kind::Dynamics, 0},
		{Record::Kit, 0x440, 16, "kit trig groups tracks 1-16", "trig groups", g_groupColor, Kind::TrigGroup, 0},
		{Record::Kit, 0x450, 16, "kit mute groups tracks 1-16", "mute groups", g_groupColor, Kind::MuteGroup, 0},
	};

	inline constexpr uint32_t g_songColor = 0xfff6b26b;
	inline constexpr uint32_t g_globalColor = 0xff9fc5e8;
	inline constexpr uint32_t g_currentColor = 0xffe06666;

	inline constexpr Field g_songFields[] = {
		{Record::Song, 0x00, 1, "song number", "song #", g_songColor, Kind::SongIndex, 0},
		{Record::Song, 0x02, 16, "song name", "song name", g_songColor, Kind::SongName, 0},
		{Record::Song, 0x12, 0x9fe, "song rows", "song rows", g_songColor, Kind::SongRows, 0},
	};

	inline constexpr Field g_globalFields[] = {
		{Record::Global, 0x00, 1, "global slot", "global #", g_globalColor, Kind::GlobalSlot, 0},
		{Record::Global, 0x01, 0xbb, "global settings", "global", g_globalColor, Kind::GlobalData, 0},
	};

#undef MD_RAM_TRIG
#undef MD_RAM_LOCK
#undef MD_RAM_KIT_TRACK
#undef MD_RAM_LFO

	inline const char* kindTitle(const Kind _kind)
	{
		switch(_kind)
		{
		case Kind::Trig: return "pattern triggers 1-32";
		case Kind::LockMask: return "pattern lock params 1-24";
		case Kind::Accent: return "pattern accent steps 1-32";
		case Kind::Slide: return "pattern slide steps 1-32";
		case Kind::Swing: return "pattern swing steps 1-32";
		case Kind::SwingAmount: return "pattern swing amount";
		case Kind::KitName: return "kit name";
		case Kind::Synth: return "synthesis parameters 1-8";
		case Kind::Effects: return "effects parameters 1-8";
		case Kind::Routing: return "routing parameters 1-8";
		case Kind::Level: return "kit track levels 1-16";
		case Kind::Model: return "kit drum models";
		case Kind::Lfo: return "LFO";
		case Kind::Reverb: return "kit reverb parameters 1-8";
		case Kind::Delay: return "kit delay parameters 1-8";
		case Kind::Eq: return "kit EQ parameters 1-8";
		case Kind::Dynamics: return "kit dynamics parameters 1-8";
		case Kind::TrigGroup: return "kit trig groups";
		case Kind::MuteGroup: return "kit mute groups";
		case Kind::PatternHeader: return "pattern header";
		case Kind::ParamLocks: return "pattern parameter locks";
		case Kind::SongIndex: return "song number";
		case Kind::SongName: return "song name";
		case Kind::SongRows: return "song rows";
		case Kind::GlobalSlot: return "global slot";
		case Kind::GlobalData: return "global settings";
		case Kind::CurrentPattern: return "current pattern";
		case Kind::CurrentKit: return "current kit";
		case Kind::CurrentSong: return "current song";
		case Kind::CurrentGlobal: return "current global";
		case Kind::None: break;
		}
		return "";
	}

	inline const Field* fieldAt(const Field* _fields, const size_t _count, const uint32_t _offset)
	{
		for(size_t i = 0; i < _count; ++i)
		{
			if(_offset >= _fields[i].offset && _offset < _fields[i].offset + _fields[i].size)
				return &_fields[i];
		}
		return nullptr;
	}

	inline const Field* patternFieldAt(const uint32_t _offset)
	{
		return fieldAt(g_patternFields, std::size(g_patternFields), _offset);
	}

	inline const Field* kitFieldAt(const uint32_t _offset)
	{
		return fieldAt(g_kitFields, std::size(g_kitFields), _offset);
	}

	inline const Field* songFieldAt(const uint32_t _offset)
	{
		return fieldAt(g_songFields, std::size(g_songFields), _offset);
	}

	inline const Field* globalFieldAt(const uint32_t _offset)
	{
		return fieldAt(g_globalFields, std::size(g_globalFields), _offset);
	}

	inline Record recordFor(const Annotate _annotate)
	{
		switch(_annotate)
		{
		case Annotate::Kit: return Record::Kit;
		case Annotate::Song: return Record::Song;
		case Annotate::Global: return Record::Global;
		case Annotate::Pattern:
		case Annotate::All:
		case Annotate::Off:
			break;
		}
		return Record::Pattern;
	}

	inline std::string slotName(const Record _record, const uint32_t _index)
	{
		switch(_record)
		{
		case Record::Pattern: return patternSlotName(_index);
		case Record::Kit: return kitSlotName(_index);
		case Record::Song: return songSlotName(_index);
		case Record::Global: return globalSlotName(_index);
		case Record::Current: break;
		}
		return {};
	}

	inline const Field* fieldFor(const Record _record, const uint32_t _offset)
	{
		switch(_record)
		{
		case Record::Pattern: return patternFieldAt(_offset);
		case Record::Kit: return kitFieldAt(_offset);
		case Record::Song: return songFieldAt(_offset);
		case Record::Global: return globalFieldAt(_offset);
		case Record::Current: break;
		}
		return nullptr;
	}

	struct Segment
	{
		ramDiff::RegionKind region = ramDiff::RegionKind::Patch;
		uint32_t address = 0;
		uint32_t size = 0;
		std::string name;
		std::string label;
	};

	inline bool parsePatternSlot(const std::string_view _text, uint32_t& _index)
	{
		if(_text.size() < 2 || _text.size() > 3)
			return false;
		const auto bank = static_cast<char>(std::toupper(static_cast<unsigned char>(_text[0])));
		if(bank < 'A' || bank > 'H')
			return false;
		unsigned number = 0;
		for(size_t i = 1; i < _text.size(); ++i)
		{
			if(_text[i] < '0' || _text[i] > '9')
				return false;
			number = number * 10 + static_cast<unsigned>(_text[i] - '0');
		}
		if(number < 1 || number > 16)
			return false;
		_index = static_cast<uint32_t>(bank - 'A') * 16 + (number - 1);
		return true;
	}

	inline bool parseIndex1(const std::string_view _text, const uint32_t _max, uint32_t& _index)
	{
		if(_text.empty())
			return false;
		unsigned number = 0;
		for(const auto ch : _text)
		{
			if(ch < '0' || ch > '9')
				return false;
			number = number * 10 + static_cast<unsigned>(ch - '0');
		}
		if(number < 1 || number > _max)
			return false;
		_index = number - 1;
		return true;
	}

	inline std::optional<Segment> segmentFromField(const Map& _map, const Record _record,
		const uint32_t _index, const Field& _field, std::string _name)
	{
		uint32_t base = UINT32_MAX;
		uint32_t stride = 0;
		uint32_t count = 1;
		switch(_record)
		{
		case Record::Pattern:
			base = _map.patternBase;
			stride = _map.patternStride;
			count = _map.patternCount;
			break;
		case Record::Kit:
			base = _map.kitBase;
			stride = _map.kitStride;
			count = _map.kitCount;
			break;
		case Record::Song:
			base = _map.songBase;
			stride = _map.songStride;
			count = _map.songCount;
			break;
		case Record::Global:
			base = _map.globalBase;
			stride = _map.globalStride;
			count = _map.globalCount;
			break;
		case Record::Current:
			break;
		}
		if(base == UINT32_MAX || _index >= count)
			return std::nullopt;
		const auto address = base + (stride == 0 ? 0 : _index * stride) + _field.offset;
		Segment segment;
		segment.address = address;
		segment.size = _field.size;
		segment.name = std::move(_name);
		const auto slot = slotName(_record, _index);
		segment.label = slot.empty() ? std::string(_field.label) : slot + " " + _field.label;
		return segment;
	}

	inline std::optional<Segment> resolveSegment(const Map& _map, const std::string& _name)
	{
		std::string parts[4];
		size_t count = 0;
		size_t start = 0;
		for(size_t i = 0; i <= _name.size() && count < 4; ++i)
		{
			if(i == _name.size() || _name[i] == ':')
			{
				parts[count++] = _name.substr(start, i - start);
				start = i + 1;
			}
		}
		if(count == 0 || parts[0].empty())
			return std::nullopt;

		const auto equals = [](const std::string& _text, const char* const _value)
		{
			return _text == _value;
		};

		if(equals(parts[0], "current") && count >= 2)
		{
			Segment segment;
			segment.size = 1;
			if(equals(parts[1], "pattern") && _map.currentPattern != UINT32_MAX)
			{
				segment.address = _map.currentPattern;
				segment.size = 2;
				segment.name = "current:pattern";
				segment.label = "current pattern";
				return segment;
			}
			if(equals(parts[1], "kit") && _map.currentKit != UINT32_MAX)
			{
				segment.address = _map.currentKit;
				segment.name = "current:kit";
				segment.label = "current kit";
				return segment;
			}
			if(equals(parts[1], "song") && _map.currentSong != UINT32_MAX)
			{
				segment.address = _map.currentSong;
				segment.name = "current:song";
				segment.label = "current song";
				return segment;
			}
			if(equals(parts[1], "global") && _map.currentGlobal != UINT32_MAX)
			{
				segment.address = _map.currentGlobal;
				segment.name = "current:global";
				segment.label = "current global";
				return segment;
			}
			return std::nullopt;
		}

		if(equals(parts[0], "pattern"))
		{
			if(count < 2)
				return std::nullopt;
			uint32_t index = 0;
			if(!parsePatternSlot(parts[1], index))
				return std::nullopt;
			if(count == 2)
			{
				if(_map.patternBase == UINT32_MAX)
					return std::nullopt;
				Segment segment;
				segment.address = _map.patternBase + index * _map.patternStride;
				segment.size = _map.patternStride == 0 ? 0x896 : _map.patternStride;
				segment.name = "pattern:" + patternSlotName(index);
				segment.label = patternSlotName(index) + " pattern";
				return segment;
			}
			const auto& fieldKey = parts[2];
			uint32_t track = 0;
			if(count >= 4 && !parseIndex1(parts[3], 16, track))
				return std::nullopt;
			const Field* field = nullptr;
			if(fieldKey == "trigs" || fieldKey == "triggers")
			{
				if(count == 3)
				{
					Field all{Record::Pattern, 0x00, 0x40, "pattern triggers 1-32 tracks 1-16",
						"trigs", g_trigColor, Kind::Trig, 0};
					return segmentFromField(_map, Record::Pattern, index, all,
						"pattern:" + patternSlotName(index) + ":trigs");
				}
				field = patternFieldAt(track * 4);
			}
			else if(fieldKey == "locks")
			{
				if(count == 3)
				{
					Field all{Record::Pattern, 0x40, 0x40, "pattern lock params tracks 1-16",
						"locks", g_lockColor, Kind::LockMask, 0};
					return segmentFromField(_map, Record::Pattern, index, all,
						"pattern:" + patternSlotName(index) + ":locks");
				}
				field = patternFieldAt(0x40 + track * 4);
			}
			else if(fieldKey == "accent")
				field = patternFieldAt(0x80);
			else if(fieldKey == "slide")
				field = patternFieldAt(0x84);
			else if(fieldKey == "swing")
				field = patternFieldAt(0x88);
			else if(fieldKey == "header")
			{
				Field header{Record::Pattern, 0x90, 6, "pattern header", "header",
					g_accentColor, Kind::PatternHeader, 0};
				return segmentFromField(_map, Record::Pattern, index, header,
					"pattern:" + patternSlotName(index) + ":header");
			}
			else if(fieldKey == "paramlocks")
				field = patternFieldAt(0x96);
			if(!field)
				return std::nullopt;
			auto name = "pattern:" + patternSlotName(index) + ":" + fieldKey;
			if(count >= 4)
				name += ":" + std::to_string(track + 1);
			return segmentFromField(_map, Record::Pattern, index, *field, std::move(name));
		}

		if(equals(parts[0], "kit"))
		{
			uint32_t index = 0;
			if(count < 2)
				return std::nullopt;
			if(parts[1] != "current" && !parseIndex1(parts[1], std::max(_map.kitCount, 1u), index))
				return std::nullopt;
			if(count == 2)
			{
				if(_map.kitBase == UINT32_MAX)
					return std::nullopt;
				Segment segment;
				segment.address = _map.kitBase + ( _map.kitStride == 0 ? 0 : index * _map.kitStride);
				segment.size = 0x460;
				segment.name = parts[1] == "current" ? "kit:current" : "kit:" + kitSlotName(index);
				segment.label = "kit working copy";
				if(parts[1] != "current")
					segment.label = kitSlotName(index);
				return segment;
			}
			const auto& fieldKey = parts[2];
			uint32_t track = 0;
			if(count >= 4 && !parseIndex1(parts[3], 16, track))
				return std::nullopt;
			const Field* field = nullptr;
			if(fieldKey == "name")
				field = kitFieldAt(0);
			else if(fieldKey == "synth" && count >= 4)
				field = kitFieldAt(0x10 + track * 24);
			else if((fieldKey == "fx" || fieldKey == "effects") && count >= 4)
				field = kitFieldAt(0x18 + track * 24);
			else if((fieldKey == "route" || fieldKey == "routing") && count >= 4)
				field = kitFieldAt(0x20 + track * 24);
			else if(fieldKey == "levels")
				field = kitFieldAt(0x190);
			else if(fieldKey == "models")
				field = kitFieldAt(0x1a0);
			else if(fieldKey == "lfo" && count >= 4)
				field = kitFieldAt(0x1e0 + track * 36);
			else if(fieldKey == "reverb")
				field = kitFieldAt(0x420);
			else if(fieldKey == "delay")
				field = kitFieldAt(0x428);
			else if(fieldKey == "eq")
				field = kitFieldAt(0x430);
			else if(fieldKey == "dynamics")
				field = kitFieldAt(0x438);
			if(!field)
				return std::nullopt;
			auto name = std::string("kit:") + (parts[1] == "current" ? "current" : kitSlotName(index));
			name += ":" + fieldKey;
			if(count >= 4)
				name += ":" + std::to_string(track + 1);
			return segmentFromField(_map, Record::Kit, index, *field, std::move(name));
		}

		if(equals(parts[0], "song"))
		{
			if(count < 2)
				return std::nullopt;
			uint32_t index = 0;
			if(!parseIndex1(parts[1], _map.songCount == 0 ? 32 : _map.songCount, index))
				return std::nullopt;
			if(count == 2)
			{
				if(_map.songBase == UINT32_MAX)
					return std::nullopt;
				Segment segment;
				segment.address = _map.songBase + index * _map.songStride;
				segment.size = _map.songStride == 0 ? 0xa10 : _map.songStride;
				char name[16];
				std::snprintf(name, sizeof(name), "song:%02u", index + 1);
				segment.name = name;
				segment.label = songSlotName(index);
				return segment;
			}
			const Field* field = nullptr;
			if(parts[2] == "name")
				field = songFieldAt(0x02);
			else if(parts[2] == "rows")
				field = songFieldAt(0x12);
			if(!field)
				return std::nullopt;
			char name[24];
			std::snprintf(name, sizeof(name), "song:%02u:%s", index + 1, parts[2].c_str());
			return segmentFromField(_map, Record::Song, index, *field, name);
		}

		if(equals(parts[0], "global"))
		{
			if(count < 2)
				return std::nullopt;
			uint32_t index = 0;
			if(!parseIndex1(parts[1], _map.globalCount == 0 ? 8 : _map.globalCount, index))
				return std::nullopt;
			if(_map.globalBase == UINT32_MAX)
				return std::nullopt;
			Segment segment;
			segment.address = _map.globalBase + index * _map.globalStride;
			segment.size = _map.globalStride == 0 ? 0xbc : _map.globalStride;
			char name[16];
			std::snprintf(name, sizeof(name), "global:%u", index + 1);
			segment.name = name;
			segment.label = globalSlotName(index);
			return segment;
		}

		return std::nullopt;
	}

	inline std::vector<Segment> listSegments(const Map& _map, const std::string& _record = {})
	{
		std::vector<Segment> segments;
		const auto want = [&](const char* const _kind)
		{
			return _record.empty() || _record == _kind;
		};
		if(want("current"))
		{
			if(auto segment = resolveSegment(_map, "current:pattern"))
				segments.push_back(*segment);
			if(auto segment = resolveSegment(_map, "current:kit"))
				segments.push_back(*segment);
			if(auto segment = resolveSegment(_map, "current:song"))
				segments.push_back(*segment);
			if(auto segment = resolveSegment(_map, "current:global"))
				segments.push_back(*segment);
		}
		if(want("pattern") && _map.patternBase != UINT32_MAX)
		{
			for(uint32_t i = 0; i < _map.patternCount; ++i)
			{
				if(auto segment = resolveSegment(_map, "pattern:" + patternSlotName(i)))
					segments.push_back(*segment);
			}
		}
		if(want("kit") && _map.kitBase != UINT32_MAX)
		{
			if(auto segment = resolveSegment(_map, "kit:current"))
				segments.push_back(*segment);
		}
		if(want("song") && _map.songBase != UINT32_MAX)
		{
			for(uint32_t i = 0; i < _map.songCount; ++i)
			{
				char name[16];
				std::snprintf(name, sizeof(name), "song:%02u", i + 1);
				if(auto segment = resolveSegment(_map, name))
					segments.push_back(*segment);
			}
		}
		if(want("global") && _map.globalBase != UINT32_MAX)
		{
			for(uint32_t i = 0; i < _map.globalCount; ++i)
			{
				char name[16];
				std::snprintf(name, sizeof(name), "global:%u", i + 1);
				if(auto segment = resolveSegment(_map, name))
					segments.push_back(*segment);
			}
		}
		return segments;
	}

	struct Hit
	{
		const Field* field = nullptr;
		uint32_t recordIndex = 0;
		uint32_t color = 0;
		std::string text;
	};

	inline std::optional<Hit> hitCurrent(const Map& _map, const uint32_t _address)
	{
		const auto make = [&](const uint32_t _base, const Kind _kind, const char* const _label)
			-> std::optional<Hit>
		{
			if(_base == UINT32_MAX || _address < _base)
				return std::nullopt;
			const auto size = _kind == Kind::CurrentPattern ? 2u : 1u;
			if(_address >= _base + size)
				return std::nullopt;
			static Field currentPattern{Record::Current, 0, 1, "current pattern", "cur pattern",
				g_currentColor, Kind::CurrentPattern, 0};
			static Field currentKit{Record::Current, 0, 1, "current kit", "cur kit",
				g_currentColor, Kind::CurrentKit, 0};
			static Field currentSong{Record::Current, 0, 1, "current song", "cur song",
				g_currentColor, Kind::CurrentSong, 0};
			static Field currentGlobal{Record::Current, 0, 1, "current global", "cur global",
				g_currentColor, Kind::CurrentGlobal, 0};
			const Field* field = &currentPattern;
			if(_kind == Kind::CurrentKit) field = &currentKit;
			else if(_kind == Kind::CurrentSong) field = &currentSong;
			else if(_kind == Kind::CurrentGlobal) field = &currentGlobal;
			(void)_label;
			return Hit{field, 0, field->color, field->label};
		};
		if(auto hit = make(_map.currentPattern, Kind::CurrentPattern, "current pattern"))
			return hit;
		if(auto hit = make(_map.currentKit, Kind::CurrentKit, "current kit"))
			return hit;
		if(auto hit = make(_map.currentSong, Kind::CurrentSong, "current song"))
			return hit;
		if(auto hit = make(_map.currentGlobal, Kind::CurrentGlobal, "current global"))
			return hit;
		return std::nullopt;
	}

	inline std::optional<Hit> hit(const Map& _map, const ramDiff::RegionKind _region,
		const uint32_t _address, const Record _annotate, const uint32_t _fallbackBase = UINT32_MAX)
	{
		if(_region != ramDiff::RegionKind::Patch && _region != ramDiff::RegionKind::Main)
			return std::nullopt;
		if(_annotate == Record::Current)
			return _region == ramDiff::RegionKind::Patch ? hitCurrent(_map, _address) : std::nullopt;

		uint32_t base = UINT32_MAX;
		uint32_t stride = 0;
		uint32_t count = 1;
		switch(_annotate)
		{
		case Record::Pattern:
			base = _map.patternBase;
			stride = _map.patternStride;
			count = _map.patternCount;
			break;
		case Record::Kit:
			base = _map.kitBase;
			stride = _map.kitStride;
			count = _map.kitCount;
			break;
		case Record::Song:
			base = _map.songBase;
			stride = _map.songStride;
			count = _map.songCount;
			break;
		case Record::Global:
			base = _map.globalBase;
			stride = _map.globalStride;
			count = _map.globalCount;
			break;
		case Record::Current:
			break;
		}
		if(base == UINT32_MAX)
			base = _fallbackBase;
		if(base == UINT32_MAX || _address < base)
			return std::nullopt;
		const auto relative = _address - base;
		uint32_t index = 0;
		uint32_t offset = relative;
		if(stride != 0)
		{
			index = relative / stride;
			if(index >= count)
				return std::nullopt;
			offset = relative % stride;
		}
		else if(count <= 1)
		{
			index = 0;
		}
		else if(relative >= 0x10000)
			return std::nullopt;
		const auto* field = fieldFor(_annotate, offset);
		if(!field)
			return std::nullopt;
		Hit result;
		result.field = field;
		result.recordIndex = index;
		result.color = field->color;
		const auto slot = slotName(_annotate, index);
		result.text = slot.empty() ? std::string(field->label) : slot + " " + field->label;
		return result;
	}

	inline std::optional<Hit> hit(const Map& _map, const ramDiff::RegionKind _region,
		const uint32_t _address, const Annotate _annotate, const uint32_t _fallbackBase = UINT32_MAX)
	{
		if(_annotate == Annotate::Off)
			return std::nullopt;
		if(_annotate == Annotate::All)
		{
			if(const auto found = hit(_map, _region, _address, Record::Current, _fallbackBase))
				return found;
			if(const auto found = hit(_map, _region, _address, Record::Pattern, _fallbackBase))
				return found;
			if(const auto found = hit(_map, _region, _address, Record::Kit, _fallbackBase))
				return found;
			if(const auto found = hit(_map, _region, _address, Record::Song, _fallbackBase))
				return found;
			return hit(_map, _region, _address, Record::Global, _fallbackBase);
		}
		if(_annotate == Annotate::Pattern || _annotate == Annotate::Kit
			|| _annotate == Annotate::Song || _annotate == Annotate::Global)
			return hit(_map, _region, _address, recordFor(_annotate), _fallbackBase);
		return std::nullopt;
	}

	inline std::string formatKindRange(const Kind _kind, const uint8_t _firstTrack, const uint8_t _lastTrack)
	{
		std::string text = kindTitle(_kind);
		if(_firstTrack == 0)
			return text;
		if(_kind == Kind::Trig || _kind == Kind::LockMask)
		{
			text += _firstTrack == _lastTrack
				? " track " + std::to_string(_firstTrack)
				: " tracks " + std::to_string(_firstTrack) + "-" + std::to_string(_lastTrack);
			return text;
		}
		if(_firstTrack == _lastTrack)
			return "track " + std::to_string(_firstTrack) + " " + std::string(kindTitle(_kind));
		return "tracks " + std::to_string(_firstTrack) + "-" + std::to_string(_lastTrack)
			+ " " + std::string(kindTitle(_kind));
	}

	inline std::optional<Hit> annotateRow(const Map& _map, const ramDiff::RegionKind _region,
		const uint32_t _rowAddress, const Annotate _annotate, const uint32_t _fallbackBase,
		const uint32_t _rowBytes = static_cast<uint32_t>(ramDiff::g_bytesPerRow))
	{
		if(_annotate == Annotate::Off)
			return std::nullopt;

		const Field* unique[4]{};
		size_t uniqueCount = 0;
		Kind kind = Kind::None;
		bool sameKind = true;
		uint8_t minTrack = 16;
		uint8_t maxTrack = 0;
		uint32_t color = 0;
		uint32_t recordIndex = 0;

		for(uint32_t i = 0; i < _rowBytes; ++i)
		{
			const auto found = hit(_map, _region, _rowAddress + i, _annotate, _fallbackBase);
			if(!found || !found->field)
				continue;
			if(uniqueCount == 0)
			{
				kind = found->field->kind;
				color = found->color;
				recordIndex = found->recordIndex;
			}
			else if(found->field->kind != kind)
				sameKind = false;

			bool seen = false;
			for(size_t u = 0; u < uniqueCount; ++u)
			{
				if(unique[u] == found->field)
				{
					seen = true;
					break;
				}
			}
			if(!seen && uniqueCount < 4)
				unique[uniqueCount++] = found->field;
			if(found->field->track != 0)
			{
				minTrack = std::min(minTrack, found->field->track);
				maxTrack = std::max(maxTrack, found->field->track);
			}
		}

		if(uniqueCount == 0)
			return std::nullopt;

		Hit result;
		result.field = unique[0];
		result.recordIndex = recordIndex;
		result.color = color;
		if(sameKind && uniqueCount > 1 && minTrack != 0 && maxTrack >= minTrack)
			result.text = formatKindRange(kind, minTrack, maxTrack);
		else if(uniqueCount == 1)
			result.text = unique[0]->label;
		else
		{
			result.text = unique[0]->shortLabel;
			for(size_t u = 1; u < uniqueCount && u < 2; ++u)
			{
				result.text += " / ";
				result.text += unique[u]->shortLabel;
			}
			if(uniqueCount > 2)
				result.text += " / …";
		}
		if(unique[0]->record != Record::Current)
		{
			const auto slot = slotName(unique[0]->record, recordIndex);
			if(!slot.empty())
				result.text = slot + " " + result.text;
		}
		return result;
	}
}
