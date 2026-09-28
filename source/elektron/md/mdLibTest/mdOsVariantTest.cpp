#include "mdLib/mdosimage.h"
#include "mdLib/mdosvariant.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace md
{
	struct DevicePreparedStateTestAccess
	{
		static Hardware* preparedHardware(Device::PreparedState& _prepared)
		{
			return _prepared.m_hardware.get();
		}
		static uint64_t liveVariantFingerprint(const Device& _device)
		{
			return _device.m_liveVariantFingerprint;
		}
		static void persistUpgradeImage(Device& _device)
		{
			_device.persistOsUpgradeImage();
		}
	};
}

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	std::vector<uint8_t> readFile(const std::string& _path)
	{
		std::vector<uint8_t> data;
		require(baseLib::filesystem::readFile(data, _path),
			("could not read " + _path).c_str());
		return data;
	}

	std::string sysexDir()
	{
		const auto* const dir = std::getenv("GEARMULATOR_MDMM_SYSEX_DIR");
		return dir && *dir ? dir : std::string{};
	}

	std::string romPath(const char* const _env)
	{
		const auto* const path = std::getenv(_env);
		return path && *path ? path : std::string{};
	}

	std::vector<uint8_t> readRom(const char* const _env,
		const md::MachineModel _model)
	{
		const auto path = romPath(_env);
		if(path.empty())
			return {};
		std::vector<uint8_t> rom;
		if(!baseLib::filesystem::readFile(rom, path)
			|| !md::RomLoader::isRomForModel(rom, _model))
			return {};
		return rom;
	}

	md::OsUpgradeDecodeResult decodeFile(const std::string& _path)
	{
		md::OsUpgradeDecodeResult result;
		require(md::decodeOsUpgradeSysex(result, readFile(_path)),
			("decode failed: " + _path).c_str());
		return result;
	}

	void assembleTests()
	{
		const auto dir = sysexDir();
		if(dir.empty())
		{
			std::cout << "mdOsVariantTest: SKIP (GEARMULATOR_MDMM_SYSEX_DIR not supplied)\n";
			return;
		}
		const auto mdRom = readRom("GEARMULATOR_MD_FIRMWARE_BIN",
			md::MachineModel::Machinedrum);
		require(!mdRom.empty(), "MD firmware image required");

		// X.13 assembles onto stock: bootloader, shared payload and user
		// space survive untouched, OS region comes from the file.
		const auto decoded = decodeFile(dir
			+ "/custom-firmware-md-mm/Machinedrum_SPS1-UW_OS_X.13.syx");
		md::OsVariant variant;
		require(md::assembleOsVariant(variant, mdRom, decoded, "X.13 test"),
			"assemble failed");
		require(variant.image.size() == md::g_romSize, "variant size");
		// Below the split, every byte outside the decoded coverage must
		// still be stock (covered bytes come from the file, including
		// regions the upgrade re-flashes identically, e.g. sector 13).
		for(uint32_t i = 0; i < md::g_variantSplitAddress; ++i)
		{
			const bool covered = i >= decoded.baseAddress && i < decoded.endAddress;
			if(!covered)
				require(variant.image[i] == mdRom[i], "uncovered byte differs");
		}
		// At/above the split the variant is byte-identical stock.
		require(std::equal(variant.image.begin() + md::g_variantSplitAddress,
			variant.image.end(), mdRom.begin() + md::g_variantSplitAddress),
			"user space leaked into variant");
		std::cout << "assembled X.13 variant fp=" << std::hex
			<< variant.fingerprint << std::dec << '\n';

		// End to end: the assembled image must equal what the firmware
		// itself programmed into live flash (the legacy sidecar).
		const auto* const sidecarPath = std::getenv("GEARMULATOR_MD_X13_FLASH_BIN");
		if(sidecarPath && *sidecarPath)
		{
			const auto sidecar = readFile(sidecarPath);
			require(sidecar.size() == md::g_romSize, "sidecar size");
			require(std::equal(variant.image.begin() + decoded.baseAddress,
				variant.image.begin() + decoded.endAddress,
				sidecar.begin() + decoded.baseAddress),
				"assembled image differs from live-flashed bytes");
			std::cout << "assembled image matches live flash over coverage\n";
		}

		// Wrong-sized stock and empty decode fail loudly.
		md::OsVariant rejected;
		require(!md::assembleOsVariant(rejected, std::vector<uint8_t>(8, 0),
			decoded, "x"), "bad stock accepted");
		md::OsUpgradeDecodeResult empty;
		require(!md::assembleOsVariant(rejected, mdRom, empty, "x"),
			"empty decode accepted");
	}

	void splitTests()
	{
		const auto* const sidecarPath = std::getenv("GEARMULATOR_MD_X13_FLASH_BIN");
		const auto mdRom = readRom("GEARMULATOR_MD_FIRMWARE_BIN",
			md::MachineModel::Machinedrum);
		if(!sidecarPath || !*sidecarPath || mdRom.empty())
		{
			std::cout << "mdOsVariantTest: split SKIP (needs GEARMULATOR_MD_X13_FLASH_BIN + MD ROM)\n";
			return;
		}
		const auto sidecar = readFile(sidecarPath);
		md::OsVariant variant;
		std::vector<uint8_t> overlay;
		require(md::splitSidecarUpgrade(variant, overlay, sidecar, mdRom,
			md::MachineModel::Machinedrum, "split test"), "split failed");
		require(overlay.size() == md::g_romSize - md::g_variantSplitAddress,
			"overlay size");
		require(std::equal(overlay.begin(), overlay.end(),
			sidecar.begin() + md::g_variantSplitAddress), "overlay content");
		// No user bytes in the variant: identical to stock at/above split.
		require(std::equal(variant.image.begin() + md::g_variantSplitAddress,
			variant.image.end(), mdRom.begin() + md::g_variantSplitAddress),
			"user bytes in variant");
		// Wrong base ROM and Monomachine refuse.
		md::OsVariant rejected;
		std::vector<uint8_t> ignored;
		auto wrong = mdRom;
		wrong[0x100] ^= 0xff;
		require(!md::splitSidecarUpgrade(rejected, ignored, sidecar, wrong,
			md::MachineModel::Machinedrum, "x"), "wrong base accepted");
		require(!md::splitSidecarUpgrade(rejected, ignored, sidecar, mdRom,
			md::MachineModel::Monomachine, "x"), "MM split accepted");
		std::cout << "split X.13 sidecar: variant fp=" << std::hex
			<< variant.fingerprint << std::dec << '\n';
	}
	std::string testHome()
	{
		// std::filesystem needs macOS 10.15; the deployment target is older.
		const char* tmp = std::getenv("TMPDIR");
		if(!tmp || !*tmp)
			tmp = std::getenv("TMP");
		if(!tmp || !*tmp)
			tmp = std::getenv("TEMP");
		if(!tmp || !*tmp)
			tmp = "/tmp";
		const auto dir = baseLib::filesystem::validatePath(tmp)
			+ "mdOsVariantTest/";
		// createDirectory reports failure for trailing-slash paths it just
		// created; existence is the real check.
		baseLib::filesystem::createDirectory(dir);
		require(baseLib::filesystem::exists(dir), "temp dir");
		return dir;
	}

	// Removes stored OS state (factory caches, hibernations and the
	// machine-local user RAM) so a boot below is determined only by what
	// the test sets up explicitly. Needed mid-test too: device teardowns
	// hibernate their last live state, and an older incarnation's
	// hibernation must not shadow a newer block's cache. (hasExtension
	// matches from the last dot, so pick real extensions here.)
	void clearStoredOsState(const std::string& _home)
	{
		for(const char* const extension : {".cache", ".hibernation", ".patchram"})
		{
			std::vector<std::string> stale;
			if(baseLib::filesystem::findFiles(stale, _home + "nvram/",
				extension, 1, 8ull << 32))
			{
				for(const auto& file : stale)
					baseLib::filesystem::remove(file);
			}
		}
	}

	void advanceToReady(md::Hardware& _hardware, const uint32_t _maxSeconds)
	{
		uint32_t frames = 0;
		const uint32_t budget = md::g_samplerate * _maxSeconds;
		while(!_hardware.isAudioReady() && frames < budget)
		{
			_hardware.advance(1024);
			frames += 1024;
		}
		require(_hardware.isAudioReady(), "machine never reached audio-ready");
	}

	synthLib::DeviceCreateParams deviceParams(const std::vector<uint8_t>& _rom,
		const std::string& _romName, const md::MachineModel _model,
		const std::string& _home, const uint64_t _variant = 0)
	{
		synthLib::DeviceCreateParams params;
		params.romData = _rom;
		params.romName = _romName;
		params.customData = md::deviceCustomData(_model);
		params.homePath = _home;
		params.osVariantFingerprint = _variant;
		return params;
	}

	void selectionTests()
	{
		const auto home = testHome();
		uint64_t fingerprint = 0;
		bool found = true;
		require(md::readOsVariantSelection(fingerprint, found, home)
			&& !found && fingerprint == 0, "fresh selection state");
		constexpr uint64_t fp = 0xefb83d7395fbe23ull;
		require(md::writeOsVariantSelection(home, fp), "write selection");
		require(md::readOsVariantSelection(fingerprint, found, home)
			&& found && fingerprint == fp, "selection round trip");
		require(md::clearOsVariantSelection(home), "clear selection");
		require(md::readOsVariantSelection(fingerprint, found, home)
			&& !found, "cleared selection reads as stock");
	}

	void bootVariant(const std::vector<uint8_t>& _rom,
		const std::string& _romName,
		const md::OsUpgradeDecodeResult& _decoded, const std::string& _home,
		const md::MachineModel _model, const char* const _label,
		uint64_t& _fingerprint)
	{
		md::OsVariant variant;
		require(md::assembleOsVariant(variant, _rom, _decoded, _label),
			"assemble failed");
		std::string filename, error;
		require(md::saveOsVariant(filename, _home, variant, error),
			error.c_str());
		auto params = deviceParams(_rom, _romName, _model, _home,
			variant.fingerprint);
		md::Device device(params);
		require(device.isValid(), "variant device invalid");
		auto& hardware = device.getHardware();
		advanceToReady(hardware, 60);
		const auto flash = hardware.copyFlashData();
		require(flash.size() == _rom.size(), "flash size moved");
		// A fresh variant boot runs the variant OS with stock user space.
		require(std::equal(flash.begin() + _decoded.baseAddress,
			flash.begin() + _decoded.endAddress,
			variant.image.begin() + _decoded.baseAddress),
			"live OS differs from variant");
		require(std::equal(flash.begin() + md::g_variantSplitAddress,
			flash.end(), _rom.begin() + md::g_variantSplitAddress),
			"user space not stock on fresh variant boot");
		std::cout << "booted variant " << _label << " fp=" << std::hex
			<< variant.fingerprint << std::dec << '\n';
		_fingerprint = variant.fingerprint;
	}

	void variantBootTests()
	{
		const auto dir = sysexDir();
		if(dir.empty())
		{
			std::cout << "mdOsVariantTest: boot SKIP (GEARMULATOR_MDMM_SYSEX_DIR not supplied)\n";
			return;
		}
		const auto mdRom = readRom("GEARMULATOR_MD_FIRMWARE_BIN",
			md::MachineModel::Machinedrum);
		const auto mmRom = readRom("GEARMULATOR_MM_FIRMWARE_BIN",
			md::MachineModel::Monomachine);
		require(!mdRom.empty() && !mmRom.empty(), "MD+MM firmware required");
		const auto mdRomPath = romPath("GEARMULATOR_MD_FIRMWARE_BIN");
		const auto mmRomPath = romPath("GEARMULATOR_MM_FIRMWARE_BIN");
		const auto home = testHome();

		// Variant factory caches, hibernations, and the stock cache from
		// previous runs would make "fresh" boots below non-fresh (device
		// teardowns hibernate). This test recreates them deterministically,
		// so start clean. The stock cache file goes too: a stale one would
		// change how the switch-test machine boots.
		clearStoredOsState(home);

		uint64_t x13fp = 0;
		const auto x13decoded = decodeFile(dir
			+ "/custom-firmware-md-mm/Machinedrum_SPS1-UW_OS_X.13.syx");
		bootVariant(mdRom, mdRomPath, x13decoded,
			home, md::MachineModel::Machinedrum, "X.13", x13fp);
		uint64_t mmfp = 0;
		bootVariant(mmRom, mmRomPath, decodeFile(dir
			+ "/official-firmware-md-mm/Elektron_Monomachine_SFX6-60_OS1.32B.syx"),
			home, md::MachineModel::Monomachine, "MM stock", mmfp);

		// The human-readable source names travel in sidecars, not in the
		// raw variant images.
		std::vector<md::OsVariantInfo> registered;
		require(md::listOsVariants(registered, home), "list variants");
		bool sawX13 = false, sawMm = false;
		for(const auto& info : registered)
		{
			if(info.fingerprint == x13fp)
				sawX13 = info.label == "X.13";
			if(info.fingerprint == mmfp)
				sawMm = info.label == "MM stock";
		}
		require(sawX13 && sawMm, "variant labels not registered");

		// Unknown selection fails the device visibly, never boots stock.
		auto params = deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, home, 0x123456789abcdef0ull);
		md::Device missing(params);
		require(!missing.isValid(), "unknown variant booted something");

		// OS switch on a live machine carries +Drive user sectors across.
		auto liveParams = deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, home);
		md::Device live(liveParams);
		require(live.isValid(), "live device invalid");
		advanceToReady(live.getHardware(), 30);
		std::string error;
		auto prepared = live.prepareOsVariantBoot(x13fp, {}, &error);
		require(prepared != nullptr, error.c_str());
		const auto* const hw = md::DevicePreparedStateTestAccess::preparedHardware(
			*prepared);
		require(hw != nullptr && hw->isValid(), "prepared machine invalid");
		const auto liveFlash = live.getHardware().copyFlashData();
		const auto newFlash = hw->copyFlashData();
		require(newFlash.size() == liveFlash.size(), "prepared flash size");
		// Prepared (pre-boot) image: variant OS + live user sectors.
		require(std::equal(newFlash.begin() + md::g_variantSplitAddress,
			newFlash.end(), liveFlash.begin() + md::g_variantSplitAddress),
			"user sectors not carried across");
		require(live.commitPreparedState(*prepared), "commit failed");
		advanceToReady(live.getHardware(), 60);
		std::cout << "variant switch committed and booted\n";

		// The legacy MIDI-upgrade slot for this ROM. A variant-running
		// machine must never write it, not even when teardown persistence
		// is forced: otherwise the variant would shadow stock on every
		// later default boot.
		char upgradeSuffix[20];
		std::snprintf(upgradeSuffix, sizeof(upgradeSuffix), "%016llx",
			static_cast<unsigned long long>(md::fingerprint(mdRom)));
		const auto upgradeFile = home + "nvram/md-os-upgrade-"
			+ upgradeSuffix + ".bin";
		md::DevicePreparedStateTestAccess::persistUpgradeImage(live);
		require(!baseLib::filesystem::exists(upgradeFile),
			"variant switch poisoned the upgrade slot");
		// Note: a headless boot never dirties flash, so this passes via the
		// unconditional variant guard, not via the dirty check. Live
		// verification (switch, quit, confirm no md-os-upgrade-*.bin) applies
		// the dirty case.

		// Variant factory cache: capture the settled machine, materialize
		// against the variant image, and persist under the variant's name.
		// Clear first: earlier teardowns hibernated older incarnations.
		clearStoredOsState(home);
		md::FactoryFlashSnapshot capture;
		std::string captureFile;
		{
			bool ready = false;
			for(int i = 0; i < 180
				&& !(ready = live.getHardware().factoryFlashCacheReady()); ++i)
				live.getHardware().advance(md::g_samplerate);
			require(ready, "variant factory capture never completed");
		}
		require(live.captureFactoryFlashCachePersistence(captureFile,
			capture, error), error.c_str());
		require(captureFile == md::variantFactoryCacheFilename(home,
			md::MachineModel::Machinedrum, x13fp),
			"variant capture routed to the stock file");
		// The live machine keeps running (and possibly writing) below;
		// pin the expected sectors before materialize consumes them.
		require(!capture.baseline.empty(), "capture has no baseline");
		const std::vector<uint8_t> capturedSectors(
			capture.baseline.begin() + md::g_variantSplitAddress,
			capture.baseline.end());
		require(md::Device::materializeFactoryFlashCache(capture,
			live.getPreparationContext(), error), error.c_str());
		require(capture.baseFingerprint == x13fp,
			"variant capture bound to the wrong base image");
		require(md::Device::writeFactoryFlashCachePersistence(captureFile,
			capture.cache, error), error.c_str());
		// Keying: the variant cache decodes against its own image only.
		std::vector<uint8_t> refused;
		require(!md::decodeFactoryFlashCache(refused, capture.cache, mdRom),
			"stock ROM accepted a variant cache");
		// A baseline matching nothing (foreign OS byte) is refused, never
		// encoded.
		md::FactoryFlashSnapshot foreign;
		foreign.baseline = live.getHardware().copyFlashData();
		foreign.baseline[md::g_variantBaseAddress] ^= 0xff;
		require(!md::Device::materializeFactoryFlashCache(foreign,
			live.getPreparationContext(), error), "foreign baseline cached");
		// A fresh boot from the cache receives the captured baseline
		// before the firmware runs a single cycle: the initialized user
		// sectors (not stock-empty ones) are already in place at t=0, so
		// there is nothing left to factory-initialize. A boot without the
		// cache file gets the raw image instead (control case).
		auto cachedParams = deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, home, x13fp);
		md::Device cached(cachedParams);
		require(cached.isValid(), "cached variant device invalid");
		const auto cachedFlash = cached.getHardware().copyFlashData();
		require(std::equal(cachedFlash.begin() + md::g_variantSplitAddress,
			cachedFlash.end(), capturedSectors.begin()),
			"cached boot did not receive the captured user sectors");
		const auto cacheMoved = captureFile + ".nocache";
		baseLib::filesystem::remove(cacheMoved);
		require(std::rename(captureFile.c_str(), cacheMoved.c_str()) == 0,
			"could not move cache file aside");
		md::Device uncached(cachedParams);
		require(uncached.isValid(), "uncached variant device invalid");
		const auto uncachedFlash = uncached.getHardware().copyFlashData();
		require(std::equal(uncachedFlash.begin() + md::g_variantSplitAddress,
			uncachedFlash.end(), mdRom.begin() + md::g_variantSplitAddress),
			"uncached boot did not fall back to the raw image");
		require(std::rename(cacheMoved.c_str(), captureFile.c_str()) == 0,
			"could not restore cache file");

		// Hibernation round-trip at file level: validated writes load
		// back, foreign images are refused both ways, stock slot included.
		md::OsVariant hibernationVariant;
		require(md::loadOsVariant(hibernationVariant,
			md::variantFilename(home, md::MachineModel::Machinedrum, x13fp),
			md::MachineModel::Machinedrum), "reload variant for hibernation");
		require(md::writeOsHibernation(home, md::MachineModel::Machinedrum,
			x13fp, hibernationVariant.image, hibernationVariant.image),
			"write variant hibernation");
		std::vector<uint8_t> hibernated;
		require(md::loadOsHibernation(hibernated, home,
			md::MachineModel::Machinedrum, x13fp, hibernationVariant.image)
			&& hibernated == hibernationVariant.image,
			"hibernation round trip");
		auto foreignImage = hibernationVariant.image;
		foreignImage[md::g_variantBaseAddress] ^= 0xff;
		require(!md::writeOsHibernation(home, md::MachineModel::Machinedrum,
			x13fp, foreignImage, hibernationVariant.image),
			"foreign image hibernated");
		require(!md::loadOsHibernation(hibernated, home,
			md::MachineModel::Machinedrum, x13fp, foreignImage),
			"hibernation loaded against foreign image");
		require(md::writeOsHibernation(home, md::MachineModel::Machinedrum,
			0, mdRom, mdRom), "write stock hibernation");
		require(md::loadOsHibernation(hibernated, home,
			md::MachineModel::Machinedrum, 0, mdRom)
			&& hibernated == mdRom, "stock hibernation round trip");
		require(!md::writeOsHibernation(home, md::MachineModel::Machinedrum,
			0, hibernationVariant.image, mdRom),
			"variant image in stock hibernation slot");
		// Stock hibernation would shadow later default boots: remove it,
		// the variant one stays for the boot-priority check below.
		require(baseLib::filesystem::remove(md::osHibernationFilename(home,
			md::MachineModel::Machinedrum, 0)), "remove stock hibernation");

		// Boot priority: hibernation beats the settled cache. Mutate only
		// user sectors (valid OS), reboot fresh, and expect the mutated
		// bytes at t=0 with no firmware involved.
		auto hibernatedImage = hibernationVariant.image;
		for(size_t i = md::g_variantSplitAddress + 0x100000;
			i < md::g_variantSplitAddress + 0x100400; ++i)
			hibernatedImage[i] ^= 0xa5;
		require(md::writeOsHibernation(home, md::MachineModel::Machinedrum,
			x13fp, hibernatedImage, hibernationVariant.image),
			"write mutated hibernation");
		md::Device hibernatedBoot(deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, home, x13fp));
		require(hibernatedBoot.isValid(), "hibernated boot invalid");
		const auto hibernatedFlash =
			hibernatedBoot.getHardware().copyFlashData();
		require(std::equal(hibernatedFlash.begin() + md::g_variantSplitAddress,
			hibernatedFlash.end(),
			hibernatedImage.begin() + md::g_variantSplitAddress),
			"fresh boot ignored hibernation");
		require(baseLib::filesystem::remove(md::osHibernationFilename(home,
			md::MachineModel::Machinedrum, x13fp)), "remove hibernation");

		// Project-state round trip on the variant machine (quit/relaunch):
		// the saved state must restore against the variant base image,
		// not stock. Re-capture first so the file baseline and the save
		// baseline are the identical snapshot; compares pre-boot images
		// exactly (committed but never advanced, so no firmware
		// housekeeping can drift the bytes).
		{
			md::FactoryFlashSnapshot fresh;
			std::string freshFile;
			require(live.captureFactoryFlashCachePersistence(freshFile,
				fresh, error), error.c_str());
			require(md::Device::materializeFactoryFlashCache(fresh,
				live.getPreparationContext(), error), error.c_str());
			require(md::Device::writeFactoryFlashCachePersistence(freshFile,
				fresh.cache, error), error.c_str());
		}
		const auto preSaveFlash = live.getHardware().copyFlashData();
		std::vector<uint8_t> projectState;
		require(live.getState(projectState, synthLib::StateTypeGlobal),
			"variant getState failed");
		auto projectRestore = md::Device::prepareState(
			live.getPreparationContext(), projectState,
			synthLib::StateTypeGlobal, {}, &error);
		require(projectRestore != nullptr, error.c_str());
		require(live.commitPreparedState(*projectRestore),
			"variant restore commit failed");
		const auto postRestoreFlash = live.getHardware().copyFlashData();
		require(std::equal(postRestoreFlash.begin() + x13decoded.baseAddress,
			postRestoreFlash.begin() + x13decoded.endAddress,
			preSaveFlash.begin() + x13decoded.baseAddress),
			"variant restore lost the variant OS");
		require(std::equal(postRestoreFlash.begin() + md::g_variantSplitAddress,
			postRestoreFlash.end(),
			preSaveFlash.begin() + md::g_variantSplitAddress),
			"variant restore lost the user sectors");
		advanceToReady(live.getHardware(), 60);
		std::cout << "variant project round trip committed and booted\n";

		// Service reboot path: a project-state prepare carrying the
		// captured variant snapshot must install the variant cache (not
		// fail against stock) and boot into it with user sectors intact.
		// This is exactly what the processor does after capturing.
		std::vector<uint8_t> liveState;
		require(live.getState(liveState, synthLib::StateTypeGlobal),
			"could not snapshot the live machine");
		auto serviceReboot = md::Device::prepareState(
			live.getPreparationContext(), liveState,
			synthLib::StateTypeGlobal, capture, &error);
		require(serviceReboot != nullptr, error.c_str());
		require(live.commitPreparedState(*serviceReboot),
			"service reboot commit failed");
		advanceToReady(live.getHardware(), 60);
		const auto rebootedFlash = live.getHardware().copyFlashData();
		md::OsVariant rebootCheck;
		require(md::loadOsVariant(rebootCheck, md::variantFilename(home,
			md::MachineModel::Machinedrum, x13fp),
			md::MachineModel::Machinedrum), "could not reload variant");
		require(std::equal(rebootedFlash.begin() + x13decoded.baseAddress,
			rebootedFlash.begin() + x13decoded.endAddress,
			rebootCheck.image.begin() + x13decoded.baseAddress),
			"service reboot lost the variant OS");
		// Boot housekeeping may touch user sectors, so exact equality
		// with the pre-reboot image is too strict; a factory reset would
		// show stock-empty sectors, which must not happen.
		require(!std::equal(rebootedFlash.begin() + md::g_variantSplitAddress,
			rebootedFlash.end(),
			mdRom.begin() + md::g_variantSplitAddress),
			"service reboot factory-reset the user sectors");
		std::cout << "service reboot committed and booted\n";

		// The Monomachine has no verified user-space split: carried sectors
		// are refused there, while whole-image variant boots stay available
		// (bootVariant above already booted the MM variant image).
		require(mmfp != 0, "MM variant fingerprint not recorded");
		auto mmParams = deviceParams(mmRom, mmRomPath,
			md::MachineModel::Monomachine, home);
		md::Device mmLive(mmParams);
		require(mmLive.isValid(), "MM live device invalid");
		advanceToReady(mmLive.getHardware(), 30);
		const std::vector<uint8_t> bogusSectors(
			md::g_romSize - md::g_variantSplitAddress, 0xa5);
		require(mmLive.prepareOsVariantBoot(mmfp, bogusSectors, &error)
			== nullptr, "MM carried sectors accepted");

		// Splice guard: blank live sectors (a firmware factory reset
		// paved them after seeing a foreign layout) must never overwrite
		// a stored base holding user data; genuine live data still wins.
		{
			md::OsVariant storedVariant;
			require(md::loadOsVariant(storedVariant, md::variantFilename(home,
				md::MachineModel::Machinedrum, x13fp),
				md::MachineModel::Machinedrum), "reload variant for guard");
			auto storedImage = storedVariant.image;
			for(size_t i = md::g_variantSplitAddress;
				i < md::g_variantSplitAddress + 0x400; ++i)
				storedImage[i] ^= 0x5a;
			require(md::writeOsHibernation(home, md::MachineModel::Machinedrum,
				x13fp, storedImage, storedVariant.image),
				"write guard hibernation");
			auto blankLive = storedVariant.image;
			std::fill(blankLive.begin() + md::g_variantSplitAddress,
				blankLive.end(), 0xff);
			auto guarded = md::Device::prepareOsVariantBoot(
				live.getPreparationContext(), blankLive, {}, x13fp, {},
				&error);
			require(guarded != nullptr, error.c_str());
			const auto guardedFlash = md::DevicePreparedStateTestAccess::preparedHardware(
				*guarded)->copyFlashData();
			require(std::equal(guardedFlash.begin() + md::g_variantSplitAddress,
				guardedFlash.end(),
				storedImage.begin() + md::g_variantSplitAddress),
				"guard failed: a wipe was carried over stored data");
			auto liveGood = storedVariant.image;
			for(size_t i = md::g_variantSplitAddress + 0x100000;
				i < md::g_variantSplitAddress + 0x100400; ++i)
				liveGood[i] ^= 0xa5;
			auto carried = md::Device::prepareOsVariantBoot(
				live.getPreparationContext(), liveGood, {}, x13fp, {},
				&error);
			require(carried != nullptr, error.c_str());
			const auto carriedFlash = md::DevicePreparedStateTestAccess::preparedHardware(
				*carried)->copyFlashData();
			require(std::equal(carriedFlash.begin() + md::g_variantSplitAddress,
				carriedFlash.end(),
				liveGood.begin() + md::g_variantSplitAddress),
				"guard failed: live data was not carried");
			std::cout << "splice guard keeps stored over blank, carries live\n";

			// Reset-to-factory case: live user sectors equal the factory
			// baseline (firmware paved foreign sectors with defaults,
			// not blank) while stored hibernation holds saves. The
			// factory baseline comes from the settled variant cache.
			md::OsVariant factoryVariant;
			require(md::loadOsVariant(factoryVariant, md::variantFilename(home,
				md::MachineModel::Machinedrum, x13fp),
				md::MachineModel::Machinedrum), "reload variant for factory");
			std::vector<uint8_t> cacheBytes;
			require(baseLib::filesystem::readFile(cacheBytes, captureFile),
				"read variant cache file");
			std::vector<uint8_t> factoryBaseline;
			require(md::decodeFactoryFlashCache(factoryBaseline, cacheBytes,
				factoryVariant.image), "decode variant cache");
			auto storedPlus = factoryBaseline;
			for(size_t i = md::g_variantSplitAddress;
				i < md::g_variantSplitAddress + 0x400; ++i)
				storedPlus[i] ^= 0x5a;
			require(md::writeOsHibernation(home, md::MachineModel::Machinedrum,
				x13fp, storedPlus, factoryVariant.image),
				"write factory-reset guard hibernation");
			auto reset = md::Device::prepareOsVariantBoot(
				live.getPreparationContext(), factoryBaseline, {}, x13fp, {},
				&error);
			require(reset != nullptr, error.c_str());
			const auto resetFlash = md::DevicePreparedStateTestAccess::preparedHardware(
				*reset)->copyFlashData();
			require(std::equal(resetFlash.begin() + md::g_variantSplitAddress,
				resetFlash.end(),
				storedPlus.begin() + md::g_variantSplitAddress),
				"guard failed: factory reset carried over stored saves");
			std::cout << "splice guard keeps stored over factory reset\n";

			// Foreign layout: live stock sectors (modified) must never
			// be spliced onto a variant target; the target firmware
			// would factory-reset them on boot. The stored base boots.
			auto stockLive = mdRom;
			for(size_t i = md::g_variantSplitAddress;
				i < md::g_variantSplitAddress + 0x400; ++i)
				stockLive[i] ^= 0x33;
			auto foreign = md::Device::prepareOsVariantBoot(
				live.getPreparationContext(), stockLive, {}, x13fp, {},
				&error);
			require(foreign != nullptr, error.c_str());
			const auto foreignFlash = md::DevicePreparedStateTestAccess::preparedHardware(
				*foreign)->copyFlashData();
			require(std::equal(foreignFlash.begin() + md::g_variantSplitAddress,
				foreignFlash.end(),
				storedPlus.begin() + md::g_variantSplitAddress),
				"guard failed: foreign sectors spliced onto variant");
			std::cout << "splice guard keeps stored over foreign layout\n";

			// Hibernation boots with the settled cache engaged: no
			// factory re-capture and no service reboot afterwards.
			auto engagedParams = deviceParams(mdRom, mdRomPath,
				md::MachineModel::Machinedrum, home, x13fp);
			md::Device engaged(engagedParams);
			require(engaged.isValid(), "engaged boot invalid");
			const auto engagedFlash = engaged.getHardware().copyFlashData();
			require(engagedFlash == storedPlus,
				"engaged boot did not run the hibernated image");
			require(engaged.getHardware().factoryFlashCacheReady(),
				"engaged boot left capture unready");
			std::cout << "hibernation boots with cache engaged\n";

			// Unsettled hibernation: a first-visit machine (no cache, no
			// prior state) hibernates immediately with zero advances. The
			// snapshot resumes init on the next boot instead of stranding
			// saves that exist only in the live machine.
			const auto home2 = home + "unsettled/";
			md::OsVariant v2image;
			require(md::loadOsVariant(v2image, md::variantFilename(home,
				md::MachineModel::Machinedrum, x13fp),
				md::MachineModel::Machinedrum), "reload variant for copy");
			std::vector<uint8_t> v2bytes;
			require(baseLib::filesystem::readFile(v2bytes,
				md::variantFilename(home, md::MachineModel::Machinedrum,
					x13fp)), "read variant file");
			baseLib::filesystem::createDirectory(
				baseLib::filesystem::getPath(md::variantFilename(home2,
					md::MachineModel::Machinedrum, x13fp)));
			require(baseLib::filesystem::writeFileAtomic(
				md::variantFilename(home2, md::MachineModel::Machinedrum,
					x13fp), v2bytes), "copy variant file");
			md::Device fresh2(deviceParams(mdRom, mdRomPath,
				md::MachineModel::Machinedrum, home2, x13fp));
			require(fresh2.isValid(), "unsettled device invalid");
			std::string hibError;
			require(fresh2.hibernateCurrentOs(&hibError), hibError.c_str());
			std::vector<uint8_t> reloaded;
			require(md::loadOsHibernation(reloaded, home2,
				md::MachineModel::Machinedrum, x13fp, v2image.image)
				&& reloaded == fresh2.getHardware().copyFlashData(),
				"unsettled hibernation mismatch");
			std::cout << "unsettled hibernation recorded\n";
		}

		// Back to default: the X.13 machine reboots into the legacy
		// resolution (stock ROM here — the isolated home has no cache
		// or persisted upgrade), proving the stock dropdown entry.
		auto backToDefault = live.prepareDefaultBoot(&error);
		require(backToDefault != nullptr, error.c_str());
		require(live.commitPreparedState(*backToDefault), "default commit failed");
		advanceToReady(live.getHardware(), 60);
		const auto stockFlash = live.getHardware().copyFlashData();
		require(stockFlash.size() == mdRom.size(), "default flash size");
		require(std::equal(stockFlash.begin() + x13decoded.baseAddress,
			stockFlash.begin() + x13decoded.endAddress,
			mdRom.begin() + x13decoded.baseAddress),
			"default boot did not revert the OS region to stock");
		std::cout << "default reboot committed and booted\n";

		// Stock project round trip still works exactly as before. The
		// isolated home has no stock cache file, so capture the settled
		// live baseline into one first (mirroring real homes).
		{
			bool ready = false;
			for(int i = 0; i < 180
				&& !(ready = live.getHardware().factoryFlashCacheReady()); ++i)
				live.getHardware().advance(md::g_samplerate);
			require(ready, "stock factory capture never completed");
			md::FactoryFlashSnapshot fresh;
			std::string freshFile;
			require(live.captureFactoryFlashCachePersistence(freshFile,
				fresh, error), error.c_str());
			require(md::Device::materializeFactoryFlashCache(fresh,
				live.getPreparationContext(), error), error.c_str());
			require(md::Device::writeFactoryFlashCachePersistence(freshFile,
				fresh.cache, error), error.c_str());
		}
		const auto stockPreFlash = live.getHardware().copyFlashData();
		std::vector<uint8_t> stockProjectState;
		require(live.getState(stockProjectState, synthLib::StateTypeGlobal),
			"stock getState failed");
		auto stockRestore = md::Device::prepareState(
			live.getPreparationContext(), stockProjectState,
			synthLib::StateTypeGlobal, {}, &error);
		require(stockRestore != nullptr, error.c_str());
		require(live.commitPreparedState(*stockRestore),
			"stock restore commit failed");
		const auto stockPostFlash = live.getHardware().copyFlashData();
		require(stockPostFlash == stockPreFlash,
			"stock restore changed the flash image");
		std::cout << "stock project round trip committed\n";

		// A variant project restored after its variant file was deleted
		// fails loudly instead of booting the wrong OS. The variant file
		// is restored afterwards (idempotent re-save).
		const auto variantFile = md::variantFilename(home,
			md::MachineModel::Machinedrum, x13fp);
		const auto variantMoved = variantFile + ".absent";
		baseLib::filesystem::remove(variantMoved);
		require(std::rename(variantFile.c_str(), variantMoved.c_str()) == 0,
			"could not move variant file aside");
		std::string absentError;
		require(md::Device::prepareState(live.getPreparationContext(),
			projectState, synthLib::StateTypeGlobal, {}, &absentError)
				== nullptr, "absent variant restored something");
		require(std::rename(variantMoved.c_str(), variantFile.c_str()) == 0,
			"could not restore variant file");
		auto revivedRestore = md::Device::prepareState(
			live.getPreparationContext(), projectState,
			synthLib::StateTypeGlobal, {}, &error);
		// The live machine is stock now, so this boots the project's own
		// variant OS from its file; the commit is not needed for the proof.
		require(revivedRestore != nullptr, error.c_str());
		std::cout << "absent variant refused, revived variant accepted\n";

		// End-to-end: none of the variant boots, switches, or forced
		// persists above leaked a shadow image into the legacy slot.
		md::DevicePreparedStateTestAccess::persistUpgradeImage(live);
		require(!baseLib::filesystem::exists(upgradeFile),
			"upgrade slot poisoned across the switch cycle");
	}
	// Saved kits, patterns and songs live in the machine's battery-backed
	// patch RAM, not in flash: the OS switch must carry that RAM into the
	// selected OS and persist it machine-locally, so no OS ever loses the
	// user's work (and switching back does not resurrect an older one).
	void userRamTests()
	{
		const auto dir = sysexDir();
		const auto mdRom = readRom("GEARMULATOR_MD_FIRMWARE_BIN",
			md::MachineModel::Machinedrum);
		if(dir.empty() || mdRom.empty())
		{
			std::cout << "mdOsVariantTest: user RAM SKIP (needs MD ROM + sysex dir)\n";
			return;
		}
		const auto mdRomPath = romPath("GEARMULATOR_MD_FIRMWARE_BIN");
		const auto home = testHome() + "userram/";
		baseLib::filesystem::createDirectory(home);
		require(baseLib::filesystem::exists(home), "user RAM temp dir");
		clearStoredOsState(home);

		md::OsVariant variant;
		require(md::assembleOsVariant(variant, mdRom, decodeFile(dir
			+ "/custom-firmware-md-mm/Machinedrum_SPS1-UW_OS_X.13.syx"),
			"X.13 test"), "assemble failed");
		std::string filename, error;
		require(md::saveOsVariant(filename, home, variant, error),
			error.c_str());

		// Settle an OS: run it until its factory capture completes and store
		// the cache, so later boots are the settled state a real user has
		// (hibernation + factory cache), not a first-visit factory init.
		// Progress is printed so a failure names the observed state.
		const auto settle = [&](const uint64_t _fingerprint,
			const uint32_t _budget)
		{
			md::Device machine(deviceParams(mdRom, mdRomPath,
				md::MachineModel::Machinedrum, home, _fingerprint));
			require(machine.isValid(), "settling device invalid");
			auto& settlingHw = machine.getHardware();
			bool ready = false;
			for(uint32_t i = 0; i < _budget
				&& !(ready = settlingHw.factoryFlashCacheReady()); ++i)
			{
				settlingHw.advance(md::g_samplerate);
				if((i + 1) % 60 == 0)
					std::fprintf(stderr,
						"[test] settle %016llx: %us dirty=%d disqualified=%d rebootable=%d\n",
						(unsigned long long)_fingerprint, i + 1,
						settlingHw.flashDirty(), settlingHw.isFactoryFlashCaptureDisqualified(),
						settlingHw.isFactoryFlashReadyForReboot());
			}
			if(!ready)
				return false;
			md::FactoryFlashSnapshot snapshot;
			std::string snapshotFile;
			require(machine.captureFactoryFlashCachePersistence(snapshotFile,
				snapshot, error), error.c_str());
			require(md::Device::materializeFactoryFlashCache(snapshot,
				machine.getPreparationContext(), error), error.c_str());
			require(md::Device::writeFactoryFlashCachePersistence(snapshotFile,
				snapshot.cache, error), error.c_str());
			return true;
		};
		require(settle(variant.fingerprint, 180), "X.13 never settled");
		// Stock settles best-effort: the switch-back below boots whatever
		// stock state exists (hibernation, cache, or the raw ROM image).
		if(!settle(0, 180))
			std::cout << "note: stock factory capture still running (soft)\n";

		md::Device live(deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, home));
		require(live.isValid(), "user RAM live device invalid");
		advanceToReady(live.getHardware(), 60);

		// Kit 1's name field sits at the start of patch RAM (offset 0x00a),
		// the same bytes the user's saved "XXX" kit occupies in a live state.
		constexpr size_t kitOffset = 0x00a;
		const std::string kitName = "TEST1";
		auto ram = live.getHardware().copyPatchRam();
		require(ram.size() == md::g_patchRamStateSize, "patch RAM size");
		std::copy(kitName.begin(), kitName.end(), ram.begin() + kitOffset);
		require(live.getHardware().getUC().replacePatchRam(ram),
			"kit stamp failed");
		require(live.getHardware().copyPatchRam() == ram, "kit stamp lost");

		// The switch snapshot carries the RAM verbatim into the prepared OS
		// (pre-boot, no firmware involved).
		auto prepared = md::Device::prepareOsVariantBoot(
			live.getPreparationContext(), live.getHardware().copyFlashData(),
			live.getHardware().copyPatchRam(), variant.fingerprint, {}, &error);
		require(prepared != nullptr, error.c_str());
		auto& preparedHw = *md::DevicePreparedStateTestAccess::preparedHardware(
			*prepared);
		require(preparedHw.isValid(), "prepared machine invalid");
		require(preparedHw.copyPatchRam() == ram,
			"kit RAM not carried into the prepared OS");
		require(md::DevicePreparedStateTestAccess::liveVariantFingerprint(live)
			== 0, "live machine was not stock");

		// Booting the real X.13 firmware from that RAM must not re-initialize
		// the kit area: the machine reaches audio ready and the saved kit name
		// is still there afterwards.
		require(live.commitPreparedState(*prepared), "variant commit failed");
		advanceToReady(live.getHardware(), 90);
		const auto x13Ram = live.getHardware().copyPatchRam();
		require(std::equal(kitName.begin(), kitName.end(),
			x13Ram.begin() + kitOffset), "X.13 boot wiped the saved kit");
		require(x13Ram.size() == md::g_patchRamStateSize, "X.13 RAM size");
		std::cout << "X.13 boot kept the carried kit RAM\n";

		// Teardown persists the machine-local RAM: a later boot of either OS
		// with no live machine (fresh instance, forced reboot) restores it.
		std::string hibernateError;
		require(live.hibernateCurrentOs(&hibernateError), hibernateError.c_str());
		const auto liveRam = live.getHardware().copyPatchRam();
		const auto ramFile = md::userPatchRamFilename(home,
			md::MachineModel::Machinedrum);
		require(!ramFile.empty() && baseLib::filesystem::exists(ramFile),
			"machine-local RAM file missing");
		std::vector<uint8_t> stored;
		require(md::loadUserPatchRam(stored, home, md::MachineModel::Machinedrum)
			&& stored == liveRam, "stored user RAM differs from the live machine");
		require(std::equal(kitName.begin(), kitName.end(),
			stored.begin() + kitOffset), "stored user RAM lost the kit");

		// The variant boot path seeds that RAM without any project state.
		md::Device seeded(deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, home, variant.fingerprint));
		require(seeded.isValid(), "seeded variant device invalid");
		require(seeded.getHardware().copyPatchRam() == stored,
			"fresh variant boot did not restore the saved kits");

		// Switching back to the default OS carries the same kits.
		auto stockPrepared = md::Device::prepareDefaultBoot(
			live.getPreparationContext(), live.getHardware().copyPatchRam(),
			&error);
		require(stockPrepared != nullptr, error.c_str());
		auto& stockHw = *md::DevicePreparedStateTestAccess::preparedHardware(
			*stockPrepared);
		require(stockHw.isValid(), "stock prepared machine invalid");
		require(stockHw.copyPatchRam() == liveRam,
			"default boot did not carry the kit RAM");
		require(live.commitPreparedState(*stockPrepared), "default commit failed");
		advanceToReady(live.getHardware(), 60);
		const auto stockRam = live.getHardware().copyPatchRam();
		require(std::equal(kitName.begin(), kitName.end(),
			stockRam.begin() + kitOffset), "back-to-default boot wiped the saved kit");
		std::cout << "user RAM carried across both OS directions\n";

		// First visit of an OS that has never run: the target initializes its
		// own flash. That factory initialization must not take the carried
		// user RAM with it, or a freshly imported OS would wipe the user's
		// kits the first time it is selected.
		const auto visitorHome = testHome() + "userramvisitor/";
		baseLib::filesystem::createDirectory(visitorHome);
		require(baseLib::filesystem::exists(visitorHome), "visitor temp dir");
		clearStoredOsState(visitorHome);
		require(md::saveOsVariant(filename, visitorHome, variant, error),
			error.c_str());
		md::Device visitor(deviceParams(mdRom, mdRomPath,
			md::MachineModel::Machinedrum, visitorHome));
		require(visitor.isValid(), "visitor live device invalid");
		advanceToReady(visitor.getHardware(), 60);
		auto visitorRam = visitor.getHardware().copyPatchRam();
		std::copy(kitName.begin(), kitName.end(), visitorRam.begin() + kitOffset);
		require(visitor.getHardware().getUC().replacePatchRam(visitorRam),
			"visitor kit stamp failed");
		auto visitorPrepared = md::Device::prepareOsVariantBoot(
			visitor.getPreparationContext(),
			visitor.getHardware().copyFlashData(), visitorRam,
			variant.fingerprint, {}, &error);
		require(visitorPrepared != nullptr, error.c_str());
		require(visitor.commitPreparedState(*visitorPrepared),
			"visitor commit failed");
		// Kits are machine state, not OS content: the switch carries the live
		// user RAM straight across, with nothing held back.
		require(visitor.getHardware().copyPatchRam() == visitorRam,
			"switch did not carry the user RAM across to the new OS");
		advanceToReady(visitor.getHardware(), 90);
		for(uint32_t i = 0; i < 300
			&& !visitor.getHardware().isFactoryFlashReadyForReboot(); ++i)
			visitor.getHardware().advance(md::g_samplerate);
		require(visitor.getHardware().isFactoryFlashReadyForReboot(),
			"first X.13 visit never finished initializing");
		const bool initKeptKits = std::equal(kitName.begin(), kitName.end(),
			visitor.getHardware().copyPatchRam().begin() + kitOffset);

		// Quitting while the target is still initializing must not turn the
		// reformatted kit store into permanent loss: the stored machine-local
		// copy stays the user's RAM (the flash slot hibernates as before, so
		// initialization can resume on the next boot).
		{
			std::string visitorHibernateError;
			require(visitor.hibernateCurrentOs(&visitorHibernateError),
				visitorHibernateError.c_str());
			std::vector<uint8_t> storedVisitor;
			require(md::loadUserPatchRam(storedVisitor, visitorHome,
				md::MachineModel::Machinedrum), "visitor user RAM not stored");
			require(std::equal(kitName.begin(), kitName.end(),
				storedVisitor.begin() + kitOffset),
				"initialization overwrote the stored user RAM");
		}

		// The processor's factory-initialization service in miniature: capture
		// the target's settled cache, then reboot the machine with the
		// preserved user RAM (serviceFactoryInitialization).
		{
			md::FactoryFlashSnapshot snapshot;
			std::string snapshotFile;
			require(visitor.captureFactoryFlashCachePersistence(snapshotFile,
				snapshot, error), error.c_str());
			require(md::Device::materializeFactoryFlashCache(snapshot,
				visitor.getPreparationContext(), error), error.c_str());
			require(md::Device::writeFactoryFlashCachePersistence(snapshotFile,
				snapshot.cache, error), error.c_str());
			std::vector<uint8_t> visitorState;
			require(visitor.getState(visitorState, synthLib::StateTypeGlobal),
				"visitor getState failed");
			auto settled = md::Device::prepareState(
				visitor.getPreparationContext(), visitorState,
				synthLib::StateTypeGlobal, snapshot, nullptr, std::nullopt,
				visitorRam);
			require(settled != nullptr, "settled reboot could not be prepared");
			require(visitor.commitPreparedState(*settled),
				"settled reboot commit failed");
		}
		advanceToReady(visitor.getHardware(), 90);
		const auto visitorX13Ram = visitor.getHardware().copyPatchRam();
		require(std::equal(kitName.begin(), kitName.end(),
			visitorX13Ram.begin() + kitOffset),
			"first X.13 visit lost the carried user RAM");
		require(visitor.getHardware().factoryFlashCacheReady(),
			"settled reboot did not engage the captured cache");
		std::cout << "first-visit factory init "
			<< (initKeptKits ? "kept" : "reformatted")
			<< " the kit store; the settled reboot reinstated the kits\n";
	}
}

int main()
{
	try
	{
		// Focused runs while iterating on one group; CI runs everything.
		const auto* const only = std::getenv("GEARMULATOR_MDMM_ONLY_TEST");
		const std::string filter = only && *only ? only : std::string{};
		const auto wanted = [&filter](const char* const _name)
		{
			return filter.empty() || filter == _name;
		};
		if(wanted("assemble"))
			assembleTests();
		if(wanted("split"))
			splitTests();
		if(wanted("selection"))
			selectionTests();
		if(wanted("userram"))
			userRamTests();
		if(wanted("variantboot"))
			variantBootTests();
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
	std::cout << "mdOsVariantTest passed\n";
	return 0;
}
