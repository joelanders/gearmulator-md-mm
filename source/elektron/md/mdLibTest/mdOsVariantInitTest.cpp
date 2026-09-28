// Does a registered OS variant reformat the live kit store on boot?
//
// expectsFactoryFlashInitialization() decides whether the user RAM is
// held back so a first-run factory initialization cannot destroy it. A
// variant boot used to be treated as always needing that initialization,
// which is only correct if the firmware really reformats. This test boots
// a real registered variant image with a recognizable kit stamp in patch
// RAM and reports whether the stamp survives, so the decision can rest on
// observed firmware behaviour rather than on the assumption that an OS
// variant is always uninitialized.
//
// Needs GEARMULATOR_MD_FIRMWARE_BIN and GEARMULATOR_MD_X13_FLASH_BIN.
#include "mdLib/mdosvariant.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

	std::string envPath(const char* const _name)
	{
		const auto* const value = std::getenv(_name);
		return value && *value ? std::string{value} : std::string{};
	}

	// A kit name is stored as plain bytes somewhere in the 1 MB user RAM.
	// Its survival is the only thing that distinguishes "the firmware left
	// the user's kits alone" from "the firmware reformatted the store".
	constexpr size_t g_kitOffset = 0x40000;
	const char g_kitName[] = "GearmulatorKitSurvivesReformatProbe";

	bool kitIntact(const std::vector<uint8_t>& _ram)
	{
		return _ram.size() > g_kitOffset + sizeof(g_kitName)
			&& std::memcmp(_ram.data() + g_kitOffset, g_kitName,
				sizeof(g_kitName)) == 0;
	}

	// Every phase boots a machine that must look like a genuine first visit, so
	// nothing a previous phase (or a previous run of this test) left behind may
	// survive.
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
}

int main()
{
	try
	{
		const auto romPath = envPath("GEARMULATOR_MD_FIRMWARE_BIN");
		const auto imagePath = envPath("GEARMULATOR_MD_X13_FLASH_BIN");
		if(romPath.empty() || imagePath.empty())
		{
			std::cout << "mdOsVariantInitTest: SKIP (needs "
				"GEARMULATOR_MD_FIRMWARE_BIN + GEARMULATOR_MD_X13_FLASH_BIN)\n";
			return 77;
		}
		const auto rom = readFile(romPath);
		const auto image = readFile(imagePath);
		require(image.size() == md::g_romSize, "variant image size");

		// Stage the real registered image as an OS variant in a scratch home,
		// so this exercises the same boot path the plugin uses.
		const char* tmp = std::getenv("TMPDIR");
		if(!tmp || !*tmp)
			tmp = "/tmp";
		const auto home = baseLib::filesystem::validatePath(tmp)
			+ "mdOsVariantInitTest/";
		baseLib::filesystem::createDirectory(home);
		// Never inherit a settled baseline or a hibernation from an earlier
		// run: this probe is about a first visit to a registered image.
		for(const char* const extension : {".cache", ".hibernation", ".patchram"})
		{
			std::vector<std::string> stale;
			if(baseLib::filesystem::findFiles(stale, home + "nvram/",
				extension, 1, 8ull << 32))
			{
				for(const auto& file : stale)
					baseLib::filesystem::remove(file);
			}
		}
		md::OsVariant variant;
		require(md::loadOsVariant(variant, imagePath,
			md::MachineModel::Machinedrum), "could not load the variant image");
		std::string filename, error;
		require(md::saveOsVariant(filename, home, variant, error), error.c_str());
		std::printf("[probe] variant fp=%016llx\n",
			static_cast<unsigned long long>(variant.fingerprint));

		synthLib::DeviceCreateParams params;
		params.romData = rom;
		params.romName = romPath;
		params.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
		params.homePath = home;
		params.osVariantFingerprint = variant.fingerprint;
		md::Device machine(params);
		require(machine.isValid(), "variant device invalid");

		// Stamp the live kit store, the way a user's own kit would appear.
		auto ram = machine.getHardware().copyPatchRam();
		require(ram.size() > g_kitOffset + sizeof(g_kitName),
			"patch RAM too small to probe");
		std::memcpy(ram.data() + g_kitOffset, g_kitName, sizeof(g_kitName));
		require(machine.getHardware().getUC().replacePatchRam(ram),
			"could not stamp the kit store");
		const bool initExpected =
			machine.getHardware().isFactoryFlashInitializationExpected();

		// Run until the settle completes, and report how much emulated time
		// that actually took: a user has to leave the machine alone for at
		// least this long on a first visit, so it needs to be a known number.
		uint32_t blocks = 0;
		while(!machine.getHardware().isFactoryFlashCacheReady() && blocks < 400)
		{
			machine.getHardware().advance(md::g_samplerate);
			++blocks;
		}
		const double emulatedSeconds =
			double(blocks) * double(md::g_samplerate) / 48000.0;
		std::printf("[probe] baseline captured after %.1fs emulated "
			"(%u blocks of %d frames)\n", emulatedSeconds, blocks,
			md::g_samplerate);

		const auto settledRam = machine.getHardware().copyPatchRam();
		const bool survived = kitIntact(settledRam);
		const auto& hw = machine.getHardware();
		std::printf("[probe] initExpected=%d flashDirty=%d cacheReady=%d "
			"readyForReboot=%d canonical=%zu kitSurvived=%d\n",
			(int)initExpected, (int)hw.flashDirty(),
			(int)hw.isFactoryFlashCacheReady(),
			(int)hw.isFactoryFlashReadyForReboot(),
			machine.canonicalUserRam().size(), (int)survived);

		// A genuine first visit to a registered image initializes: the
		// firmware writes flash, reformats the kit store, and the settle
		// captures a reusable baseline. Without that baseline the machine is
		// flagged as initializing forever and permanently refuses SysEx
		// import, so the capture completing is the property worth pinning.
		require(hw.flashDirty(),
			"a first visit to a registered image never dirtied flash, so the "
			"factory baseline can never be captured");
		require(hw.isFactoryFlashCacheReady(),
			"a first visit to a registered image never completed its factory "
			"baseline capture, leaving the OS permanently unsettled");
		require(!survived,
			"the kit store survived a first visit, so this firmware no longer "
			"reformats the store it is given");
		// The firmware does reformat whatever store it is handed at startup.
		// That is why the host keeps its own copy, so a reformat costs a
		// restore rather than the user's kits.
		std::printf("[probe] the firmware reformats the kit store it is "
			"given; the host's copy is what makes that recoverable\n");

		// Restoring project state onto a machine that is still doing its
		// first-run initialization must not quietly settle it. The restored
		// machine's flash no longer matches the registered image byte for
		// byte, so identifying it as a variant by content fails; the
		// still-initializing property has to come from the machine being
		// replaced. Otherwise the settle never runs, no baseline is ever
		// captured, and SysEx import stays refused for that OS forever.
		{
			clearStoredOsState(home);
			md::Device restoring(params);
			require(restoring.isValid(), "restore-source device invalid");
			auto& sourceHw = restoring.getHardware();
			require(sourceHw.isFactoryFlashInitializationExpected(),
				"the first visit to a registered image was not flagged as "
				"initializing, so this test is not exercising the settle");
			// Let the firmware dirty flash, which is what makes the machine's
			// image differ from the pristine registered one.
			for(uint32_t i = 0; i < 200; ++i)
				sourceHw.advance(md::g_samplerate);

			std::vector<uint8_t> state;
			require(restoring.getState(state, synthLib::StateTypeGlobal),
				"could not capture project state");
			std::printf("[probe] restoring state onto an initializing machine: "
				"initExpectedBefore=%d\n",
				(int)sourceHw.isFactoryFlashInitializationExpected());
			require(restoring.setState(state, synthLib::StateTypeGlobal),
				"restoring the captured state failed");
			const bool after =
				restoring.getHardware().isFactoryFlashInitializationExpected();
			// Did the restore keep this OS, or quietly revert the machine to
			// stock? A host that saves and restores its session without a full
			// flash payload falls back to the stock factory cache, which would
			// replace X.13 with a stock machine behind the user's back.
			const auto restoredFlash =
				restoring.getHardware().copyFlashData();
			const bool sameOs = restoredFlash.size() == variant.image.size()
				&& std::equal(variant.image.begin() + md::g_variantBaseAddress,
					variant.image.begin() + md::g_variantSplitAddress,
					restoredFlash.begin() + md::g_variantBaseAddress);
			std::printf("[probe] after restore: initExpected=%d sameOs=%d "
				"patchRam=%zu\n", (int)after, (int)sameOs,
				restoring.getHardware().copyPatchRam().size());
			require(sameOs,
				"restoring project state silently replaced the registered OS "
				"with a different one");
			require(after,
				"restoring project state silently settled a machine that was "
				"still initializing: the factory baseline can never be "
				"captured and SysEx import stays refused for this OS");
		}

		// The phase above found a kit store in RAM and reformatted it. The
		// plugin's hold-back instead hands the machine completely empty RAM,
		// so that is the case worth measuring: if the firmware only writes
		// flash when it has a store to migrate, the hold-back suppresses the
		// very write whose capture completes the settle, and the wait for a
		// baseline can never end.
		{
			clearStoredOsState(home);
			md::Device emptyRam(params);
			require(emptyRam.isValid(), "empty-RAM device invalid");
			auto& emptyHw = emptyRam.getHardware();
			uint32_t waited = 0;
			while(!emptyHw.isFactoryFlashCacheReady() && waited < 400)
			{
				emptyHw.advance(md::g_samplerate);
				++waited;
			}
			std::printf("[probe] empty-RAM boot: dirty=%d cacheReady=%d "
				"after %.1fs\n", (int)emptyHw.flashDirty(),
				(int)emptyHw.isFactoryFlashCacheReady(),
				double(waited) * double(md::g_samplerate) / 48000.0);
			if(!emptyHw.flashDirty())
			{
				std::printf("[probe] FAIL: with empty user RAM the firmware never "
					"writes flash, so holding the user's RAM back prevents the "
					"baseline capture that is supposed to reclaim it\n");
				return 1;
			}
		}

		// The firmware's one-time preparation is tied to the +Drive store, so
		// an already populated store may be exactly why a real machine never
		// starts it. Reproduce that before concluding the feature is broken.
		{
			const auto plusDrive = envPath("GEARMULATOR_MD_PLUSDRIVE_BIN");
			if(!plusDrive.empty())
			{
				for(const char* const extension : {".cache", ".hibernation", ".patchram"})
				{
					std::vector<std::string> stale;
					if(baseLib::filesystem::findFiles(stale, home + "nvram/",
						extension, 1, 8ull << 32))
					{
						for(const auto& file : stale)
							baseLib::filesystem::remove(file);
					}
				}
				const auto store = readFile(plusDrive);
				require(baseLib::filesystem::writeFile(home + "nvram/"
					+ "md-plusdrive-v1.bin", store), "could not seed +Drive store");
				std::printf("[probe] seeded a %zu byte +Drive store\n", store.size());
				md::Device populated(params);
				require(populated.isValid(), "populated-store device invalid");
				auto& populatedHw = populated.getHardware();
				uint32_t waited = 0;
				while(!populatedHw.isFactoryFlashCacheReady() && waited < 400)
				{
					populatedHw.advance(md::g_samplerate);
					++waited;
				}
				std::printf("[probe] populated-+Drive boot: dirty=%d cacheReady=%d "
					"after %.1fs\n", (int)populatedHw.flashDirty(),
					(int)populatedHw.isFactoryFlashCacheReady(),
					double(waited) * double(md::g_samplerate) / 48000.0);
				if(!populatedHw.flashDirty())
				{
					std::printf("[probe] FAIL: a populated +Drive store stops the "
						"firmware writing flash, so the baseline can never be "
						"captured and the OS stays un-settled forever\n");
					return 1;
				}
			}
			else
			{
				std::printf("[probe] no GEARMULATOR_MD_PLUSDRIVE_BIN supplied, "
					"skipped the populated-store case\n");
			}
		}

		// The whole point of the host-owned store: booting a variant hands the
		// machine the user's kits immediately, with nothing withheld and nothing
		// to wait for. A first visit must not be a special case.
		{
			clearStoredOsState(home);
			std::vector<uint8_t> storedKit(1024 * 1024, 0);
			std::memcpy(storedKit.data() + g_kitOffset, g_kitName,
				sizeof(g_kitName));
			require(baseLib::filesystem::writeFile(home + "nvram/"
				+ "md-user.patchram", storedKit), "could not store a kit");

			md::Device firstVisit(params);
			require(firstVisit.isValid(), "first-visit device invalid");
			require(kitIntact(firstVisit.getHardware().copyPatchRam()),
				"a first visit to a new OS did not present the user's kits: "
				"switching OS must not hide or withhold saved data");
			require(!firstVisit.userRamWasWiped(),
				"a freshly booted machine reports its kit store as wiped");
			require(!firstVisit.restoreUserRamIfWiped(),
				"a healthy kit store was needlessly overwritten from the "
				"host's copy, which would discard the user's edits");
			std::printf("[probe] first visit: kits present, nothing withheld\n");

			// If the firmware reformats the store it finds at startup, the host
			// puts the kits back rather than losing them.
			auto& hwRef = firstVisit.getHardware();
			std::vector<uint8_t> blank(1024 * 1024, 0);
			require(hwRef.getUC().replacePatchRam(blank),
				"could not blank the kit store to simulate a reformat");
			require(firstVisit.userRamWasWiped(),
				"a reformatted kit store was not detected");
			require(firstVisit.restoreUserRamIfWiped(),
				"the host could not restore a reformatted kit store");
			require(kitIntact(firstVisit.getHardware().copyPatchRam()),
				"the restored kit store does not contain the user's kit");
			require(!firstVisit.userRamWasWiped(),
				"the store still reads as wiped after a restore");
			std::printf("[probe] reformatted kit store restored from the host's "
				"copy\n");
		}


		std::printf("mdOsVariantInitTest passed\n");
		return 0;
	}
	catch(const std::exception& _e)
	{
		std::fprintf(stderr, "mdOsVariantInitTest: FAILED: %s\n", _e.what());
		return 1;
	}
}
