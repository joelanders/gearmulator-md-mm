#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mdosimage.h"
#include "mdtypes.h"

namespace md
{
	// OS variant management: named, immutable, fingerprint-identified full flash
	// images built from upgrade SysEx files. Booting a variant needs no MIDI
	// flashing, because the firmware cannot tell bytes it programmed itself from
	// identical bytes placed at boot.
	//
	// Verified flash map (Machinedrum, stock 1.63 ROM vs the X.13 sidecar):
	//   [0x000000, 0x004000)  bootloader, never touched by upgrades
	//   [0x004000, 0x100000)  OS code (sector 13, constant across versions)
	//   [0x100000, 0x200000)  shared payload, identical across versions
	//   [0x200000, 0x800000)  user space (+Drive kits/patterns/samples)
	// A variant covers the whole 8 MiB: stock base plus the decoded overlay
	// below the split. User bytes above the split stay out of it, so an old
	// sidecar's recorded kits can never freeze into an OS file.
	// Contains no vendor material.
	constexpr uint32_t g_variantSplitAddress = 0x00200000;
	constexpr uint32_t g_variantBaseAddress = 0x00004000;

	struct OsVariant
	{
		MachineModel model = MachineModel::Machinedrum;
		uint64_t fingerprint = 0;	// FNV-1a over the full image, identity
		std::vector<uint8_t> image;	// full g_romSize flash image
		std::string sourceName;		// upgrade file it was built from
	};

	// Overlays decoded chunks onto a copy of the stock ROM. Fails when the
	// coverage does not start at 0x4000, runs past the split address, or
	// leaves the image size wrong. The stock base preserves the bootloader,
	// the shared payload, and everything above the split untouched.
	bool assembleOsVariant(OsVariant& _variant,
		const std::vector<uint8_t>& _stockRom,
		const OsUpgradeDecodeResult& _decoded, const std::string& _sourceName);

	// Splits a legacy live-flash sidecar (OS upgrade persistence) into a
	// variant (bootloader through shared payload, user space restored from
	// the stock ROM it was upgraded from) and a user overlay (at/above the
	// split) for one-time migration. Verifies the sidecar really is based on
	// the given stock ROM (bootloader + shared payload identical) and refuses
	// anything else. Monomachine layout is not verified yet: refuses anything
	// but Machinedrum.
	bool splitSidecarUpgrade(OsVariant& _variant, std::vector<uint8_t>& _overlay,
		const std::vector<uint8_t>& _sidecar, const std::vector<uint8_t>& _stockRom,
		MachineModel _model, const std::string& _sourceName);

	// Variant file layout under a home path: os-variants/<model>-<fp>.bin.
	// Files are immutable once written (exclusive create wins).
	std::string variantFilename(const std::string& _homePath,
		MachineModel _model, uint64_t _fingerprint);
	bool saveOsVariant(std::string& _filename, const std::string& _homePath,
		const OsVariant& _variant, std::string& _error);
	bool loadOsVariant(OsVariant& _variant, const std::string& _filename,
		MachineModel _model);

	struct OsVariantInfo
	{
		MachineModel model = MachineModel::Machinedrum;
		uint64_t fingerprint = 0;
		std::string filename;
		// Human-readable source name (e.g. the imported .syx basename).
		// Best-effort sidecar: empty when the variant was registered
		// without a label or the sidecar is missing/unreadable.
		std::string label;
	};

	// Lists registered variants (by filename convention only, no manifest).
	bool listOsVariants(std::vector<OsVariantInfo>& _variants,
		const std::string& _homePath);

	// Factory cache file for one variant: nvram/<model>-<fp>-factory-v1.cache.
	// Keyed by fingerprint (never by label), so a stock cache can never be
	// mistaken for a variant one and vice versa.
	std::string variantFactoryCacheFilename(const std::string& _homePath,
		MachineModel _model, uint64_t _fingerprint);

	// Hibernation slots: one whole-flash snapshot per OS (stock uses fingerprint
	// zero), written on switch-away and teardown so every OS remembers its own
	// last state including user space (+Drive). The two OSes then keep their own
	// user-space formats instead of trampling each other's stored data. Boot
	// prefers hibernation, then factory cache, then the raw image. Validation is
	// by size plus OS-region match against the expected image, so corrupt or
	// foreign states can never boot and it falls through to the next source.
	std::string osHibernationFilename(const std::string& _homePath,
		MachineModel _model, uint64_t _fingerprint);
	bool loadOsHibernation(std::vector<uint8_t>& _flash,
		const std::string& _homePath, MachineModel _model,
		uint64_t _fingerprint, const std::vector<uint8_t>& _expectedImage);
	bool writeOsHibernation(const std::string& _homePath, MachineModel _model,
		uint64_t _fingerprint, const std::vector<uint8_t>& _flash,
		const std::vector<uint8_t>& _expectedImage);
	// Machine-local user RAM sidecar. MD kits, patterns and songs live in
	// battery-backed patch RAM, which belongs to the machine rather than the OS
	// (an OS update never clears a kit), so one file per home path serves every
	// OS: the same kits stay visible whichever OS runs, and switching back never
	// resurrects an older snapshot. Written on switch-away and teardown, read
	// when a boot has no live machine to carry the RAM from. Validated by size
	// only (g_patchRamStateSize); a missing file means the historical empty-RAM
	// factory boot.
	std::string userPatchRamFilename(const std::string& _homePath,
		MachineModel _model);
	bool loadUserPatchRam(std::vector<uint8_t>& _patchRam,
		const std::string& _homePath, MachineModel _model);
	bool writeUserPatchRam(const std::string& _homePath, MachineModel _model,
		const std::vector<uint8_t>& _patchRam);
	// Finds the registered variant whose OS region
	// [g_variantBaseAddress, g_variantSplitAddress) matches the image.
	// Content-based provenance: works no matter how the image started
	// running (fresh boot, live switch, project restore). Returns false
	// when nothing matches; teardown-only cost, never the audio thread.
	bool findOsVariantByImage(uint64_t& _fingerprint,
		const std::string& _homePath, MachineModel _model,
		const std::vector<uint8_t>& _image);
	// Same, but identifies an already-encoded factory cache by trial
	// decode against each registered image (rare path only).
	bool findOsVariantByCache(uint64_t& _fingerprint,
		const std::string& _homePath, MachineModel _model,
		const std::vector<uint8_t>& _cache);

	// Last-selected variant persistence (one hex fingerprint per home path;
	// missing file means stock). Written when the user picks an OS, read on
	// every fresh boot. Project states stay self-contained and override it.
	std::string variantSelectionFilename(const std::string& _homePath);
	bool readOsVariantSelection(uint64_t& _fingerprint, bool& _found,
		const std::string& _homePath);
	bool writeOsVariantSelection(const std::string& _homePath,
		uint64_t _fingerprint);
	bool clearOsVariantSelection(const std::string& _homePath);
}
