#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "mdtypes.h"

namespace md::automation
{
	// Stable host-parameter pages. The page numbers are part of the JUCE
	// parameter IDs, so append new pages rather than renumbering these.
	namespace machinedrum
	{
		constexpr uint8_t Synthesis = 0;
		constexpr uint8_t Effects = 1;
		constexpr uint8_t Routing = 2;
		constexpr uint8_t Level = 3;
		constexpr uint8_t Mute = 4;
		constexpr uint8_t TrackCount = 16;
	}

	namespace monomachine
	{
		constexpr uint8_t Synthesis = 0;
		constexpr uint8_t Amplification = 1;
		constexpr uint8_t Filter = 2;
		constexpr uint8_t Effects = 3;
		constexpr uint8_t Lfo1 = 4;
		constexpr uint8_t Lfo2 = 5;
		constexpr uint8_t Lfo3 = 6;
		constexpr uint8_t Level = 7;
		constexpr uint8_t Mute = 8;
		constexpr uint8_t TrackCount = 6;
	}

	struct ParameterChange
	{
		uint8_t page = 0;
		uint8_t track = 0;
		uint8_t index = 0;
		uint8_t value = 0;

		constexpr bool operator==(const ParameterChange& _other) const
		{
			return page == _other.page && track == _other.track
				&& index == _other.index && value == _other.value;
		}
	};

	using ControlChange = std::array<uint8_t, 3>;

	// baseChannel is zero-based. MD consumes four consecutive channels and MM
	// consumes six, matching the public Elektron MIDI implementations.
	std::optional<ControlChange> encodeParameterChange(MachineModel _model,
		const ParameterChange& _change, uint8_t _baseChannel);

	std::optional<ParameterChange> decodeParameterChange(MachineModel _model,
		const ControlChange& _message, uint8_t _baseChannel);
}
