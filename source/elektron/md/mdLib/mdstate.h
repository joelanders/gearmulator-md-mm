#pragma once

#include <cstdint>
#include <vector>

#include "mdtypes.h"

#include "synthLib/deviceTypes.h"

namespace md
{
	constexpr uint32_t g_patchRamStateSize = 0x100000;

	// Append a self-describing patch-RAM image to _state. The synthLib plugin wrapper may
	// already have placed its own two-byte envelope in the vector, so this function does
	// not clear it.
	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		MachineModel _model, synthLib::StateType _type);

	// Validate and decode a device payload. No output is changed on failure.
	bool decodeState(std::vector<uint8_t>& _patchRam, const std::vector<uint8_t>& _state,
		MachineModel _expectedModel, synthLib::StateType _expectedType);
}
