#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "mdtypes.h"

namespace md::midiProtocol
{
	// The payload of an Elektron SET STATUS message, without the F0/F7 wrapper so
	// it can be handed straight to juce::MidiMessage::createSysExMessage.
	using SysexBody = std::array<uint8_t, 8>;

	// Elektron command 0x71 is SET STATUS on both machines; only the product id differs.
	constexpr SysexBody setStatus(const MachineModel _model, const uint8_t _parameter, const uint8_t _value)
	{
		const auto product = _model == MachineModel::Monomachine ? uint8_t{0x03} : uint8_t{0x02};
		return { 0x00, 0x20, 0x3c, product, 0x00, 0x71, _parameter, _value };
	}

	// Appendix C: 0x01 current global, 0x02 current kit, 0x04 current pattern,
	// 0x08 current song, 0x22 current track.
	constexpr SysexBody selectGlobal(const int _slot)
	{
		return setStatus(MachineModel::Machinedrum, 0x01, static_cast<uint8_t>(std::clamp(_slot, 0, 7)));
	}

	constexpr SysexBody selectKit(const int _kit)
	{
		return setStatus(MachineModel::Machinedrum, 0x02, static_cast<uint8_t>(std::clamp(_kit, 0, 63)));
	}

	constexpr SysexBody selectPattern(const MachineModel _model, const int _pattern)
	{
		return setStatus(_model, 0x04, static_cast<uint8_t>(std::clamp(_pattern, 0, 127)));
	}

	constexpr SysexBody selectSong(const int _song)
	{
		return setStatus(MachineModel::Machinedrum, 0x08, static_cast<uint8_t>(std::clamp(_song, 0, 31)));
	}

	constexpr SysexBody selectTrack(const int _track)
	{
		return setStatus(MachineModel::Machinedrum, 0x22, static_cast<uint8_t>(std::clamp(_track, 0, 15)));
	}
}
