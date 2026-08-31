#pragma once

#include <cstdint>
#include <vector>

#include "mdtypes.h"

#include "synthLib/deviceTypes.h"

namespace md
{
	constexpr uint32_t g_patchRamStateSize = 0x100000;
	constexpr uint32_t g_mmUserFlashStateSize = 0x200000;
	constexpr uint32_t g_uwFlashSectorSize = 0x10000;

	struct DecodedState
	{
		std::vector<uint8_t> patchRam;
		std::vector<uint8_t> flashData;
		bool containsFlash = false;
	};

	// Append a self-describing patch-RAM image to _state. The synthLib plugin wrapper may
	// already have placed its own two-byte envelope in the vector, so this function does
	// not clear it.
	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		MachineModel _model, synthLib::StateType _type,
		const std::vector<uint8_t>& _userFlash = {});

	// Machinedrum UW state extends the patch-RAM snapshot with complete changed
	// flash sectors relative to the immutable firmware image. Keeping the baseline
	// outside the state avoids embedding copyrighted firmware while making every
	// user-programmed sector project-owned and independent of machine-local caches.
	// DSP RAM-machine recordings remain volatile, matching the hardware: users can
	// copy a recording to a ROM slot when it must survive a reboot/project reload.
	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _flashBaseline,
		MachineModel _model, synthLib::StateType _type);

	// Validate and decode a device payload. No output is changed on failure.
	bool decodeState(std::vector<uint8_t>& _patchRam, const std::vector<uint8_t>& _state,
		MachineModel _expectedModel, synthLib::StateType _expectedType);
	bool decodeState(std::vector<uint8_t>& _patchRam, std::vector<uint8_t>& _userFlash,
		const std::vector<uint8_t>& _state, MachineModel _expectedModel,
		synthLib::StateType _expectedType);

	// Decode either the legacy version-1 patch-RAM format or version 3. Version 3
	// reconstructs flash from _flashBaseline plus its validated sparse sector set.
	// No output is changed on failure.
	bool decodeState(DecodedState& _decoded, const std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _flashBaseline,
		MachineModel _expectedModel, synthLib::StateType _expectedType);

	// A machine-local full-flash cache is only a boot optimization. Its format is
	// deliberately separate from the project-owned sparse MDST v3 payload.
	bool encodeFactoryFlashCache(std::vector<uint8_t>& _cache,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _romBaseline);
	bool decodeFactoryFlashCache(std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _cache,
		const std::vector<uint8_t>& _romBaseline);
}
