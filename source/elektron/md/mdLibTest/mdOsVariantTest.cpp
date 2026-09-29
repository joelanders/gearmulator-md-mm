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
	}
	catch(const std::exception& e)
	{
		std::cerr << e.what() << '\n';
		return 1;
	}
	std::cout << "mdOsVariantTest passed\n";
	return 0;
}
