#pragma once

#include <cstdint>
#include <vector>

#include "mdtypes.h"

#include "synthLib/deviceTypes.h"

namespace md
{
	constexpr uint32_t g_patchRamStateSize = 0x100000;
	constexpr uint32_t g_uwFlashSectorSize = 0x10000;
	constexpr uint32_t g_mmUserFlashStateSize = 0x200000;

	struct FlashSectorOverlay
	{
		uint64_t romFingerprint = 0;
		uint64_t baselineFingerprint = 0;
		uint32_t flashSize = 0;
		std::vector<uint16_t> sectors;
		std::vector<uint8_t> data;
		bool valid = false;
	};

	struct DecodedState
	{
		std::vector<uint8_t> patchRam;
		std::vector<uint8_t> userFlash;
		FlashSectorOverlay flashOverlay;
		bool containsFlash = false;
	};

	// Append a self-describing patch-RAM image to _state. The synthLib plugin wrapper may
	// already have placed its own two-byte envelope in the vector, so this function does
	// not clear it.
	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		MachineModel _model, synthLib::StateType _type);
	// Version 2 extends Monomachine state with its private 2 MiB DigiPRO flash
	// window. Version-1 patch-only states remain readable.
	bool encodeState(std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _patchRam, MachineModel _model,
		synthLib::StateType _type, const std::vector<uint8_t>& _userFlash);

	// Machinedrum UW state extends the patch-RAM snapshot with sectors that differ
	// from the initialized factory-flash baseline. The matching ROM and factory
	// baseline are identified by fingerprints and remain outside project state.
	// Before a local factory baseline exists, state carries a complete sector image.
	// This also represents sectors erased back to ROM bytes and can restore without
	// waiting for a new machine to repeat factory initialization.
	// DSP RAM-machine recordings remain volatile, matching the hardware: users can
	// copy a recording to a ROM slot when it must survive a reboot/project reload.
	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _factoryFlashBaseline,
		const std::vector<uint8_t>& _romBaseline,
		MachineModel _model, synthLib::StateType _type);
	// Use this variant when a validated machine-local factory cache exists. It
	// remains sparse even when the initialized bytes happen to equal the ROM;
	// byte equality alone must not be mistaken for the no-cache fallback case.
	bool encodeStateWithFactoryBaseline(std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _patchRam,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _factoryFlashBaseline,
		const std::vector<uint8_t>& _romBaseline,
		MachineModel _model, synthLib::StateType _type);
	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		const FlashSectorOverlay& _flashOverlay,
		const std::vector<uint8_t>& _romBaseline,
		MachineModel _model, synthLib::StateType _type);

	// Validate and decode a device payload. No output is changed on failure.
	bool decodeState(std::vector<uint8_t>& _patchRam, const std::vector<uint8_t>& _state,
		MachineModel _expectedModel, synthLib::StateType _expectedType);

	// Decode legacy version-1 patch RAM, version-2 MM patch RAM plus user flash,
	// or the version-3/4 MD sparse-flash formats. An MD overlay remains deferred
	// until its factory baseline is available.
	// No output is changed on failure.
	bool decodeState(DecodedState& _decoded, const std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _romBaseline,
		MachineModel _expectedModel, synthLib::StateType _expectedType);
	bool applyFlashOverlay(std::vector<uint8_t>& _flashData,
		const FlashSectorOverlay& _overlay,
		const std::vector<uint8_t>& _factoryFlashBaseline,
		const std::vector<uint8_t>& _romBaseline = {});

	// The machine-local factory cache stores only initialized sectors that differ
	// from the matching ROM. Its format is separate from project state.
	bool encodeFactoryFlashCache(std::vector<uint8_t>& _cache,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _romBaseline);
	bool decodeFactoryFlashCache(std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _cache,
		const std::vector<uint8_t>& _romBaseline);
}
