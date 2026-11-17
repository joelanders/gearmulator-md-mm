#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdplusdrive.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace md
{
	struct DevicePreparedStateTestAccess
	{
		static Hardware* deferredHardware(Device& _device)
		{
			return _device.m_deferredPreparedState
				? _device.m_deferredPreparedState->m_hardware.get() : nullptr;
		}
	};
}

namespace
{
	int fail(const char* const _message)
	{
		std::cerr << "mdPlusDriveFirmwareTest: " << _message << '\n';
		return 1;
	}

	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		constexpr uint32_t block = 128;
		for(uint32_t frames = 0; frames < _frames; frames += block)
			_hardware.advance(std::min(block, _frames - frames));
	}

	bool initializeUwFlash(md::Hardware& _hardware)
	{
		constexpr uint32_t block = 128;
		constexpr uint32_t maximumFrames = md::g_samplerate * 18;
		for(uint32_t frames = 0; frames < maximumFrames
			&& !_hardware.isFactoryFlashReadyForReboot(); frames += block)
			_hardware.advance(std::min(block, maximumFrames - frames));
		return _hardware.isFactoryFlashReadyForReboot();
	}

	bool sameLcd(const md::FrontPanel& _a, const md::FrontPanel& _b)
	{
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				if(_a.getLcdPixel(x, y) != _b.getLcdPixel(x, y))
					return false;
		return true;
	}

	bool longPressChord(md::Hardware& _hardware, const md::PanelControl _first,
		const md::PanelControl _second)
	{
		const auto first = md::panelPacket(md::MachineModel::Machinedrum, _first);
		const auto second = md::panelPacket(md::MachineModel::Machinedrum, _second);
		if(!first || !second)
			return false;
		md::PanelRowState rows;
		const auto firstPress = rows.press(*first);
		_hardware.sendPanelEvent(firstPress.row, firstPress.mask);
		const auto secondPress = rows.press(*second);
		_hardware.sendPanelEvent(secondPress.row, secondPress.mask);
		advance(_hardware, md::g_samplerate * 2);
		const auto secondRelease = rows.release(*second);
		_hardware.sendPanelEvent(secondRelease.row, secondRelease.mask);
		const auto firstRelease = rows.release(*first);
		_hardware.sendPanelEvent(firstRelease.row, firstRelease.mask);
		advance(_hardware, 8192);
		return true;
	}

	bool tap(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(md::MachineModel::Machinedrum, _control);
		if(!packet)
			return false;
		_hardware.sendPanelEvent(packet->row, packet->mask);
		advance(_hardware, 2048);
		_hardware.sendPanelEvent(packet->row, 0);
		advance(_hardware, 4096);
		return true;
	}

	const md::PlusDrive& drive(md::Hardware& _hardware)
	{
		return _hardware.getUC().getSim().getPlusDrive();
	}

	synthLib::DeviceCreateParams deviceParams(const std::vector<uint8_t>& _rom,
		const std::string& _romName, const std::string& _homePath)
	{
		synthLib::DeviceCreateParams params;
		params.romData = _rom;
		params.romName = _romName;
		params.homePath = _homePath;
		params.customData = md::deviceCustomData(md::MachineModel::Machinedrum);
		return params;
	}

	void report(const char* const _label, md::Hardware& _hardware)
	{
		std::cout << _label << ": " << drive(_hardware).storedSectorCount()
			<< " sectors, " << drive(_hardware).commandCount() << " commands, last CMD"
			<< static_cast<uint32_t>(drive(_hardware).lastCommand())
			<< (drive(_hardware).initialized() ? ", initialized" : ", not initialized")
			<< '\n';
	}
}

static int runFirmwareTest(const char* const _firmwarePath, const std::string& _home)
{
	std::ifstream input(_firmwarePath, std::ios::binary);
	const std::vector<uint8_t> rom{
		std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

	const auto blankDrive = md::PlusDrive().copyStorage();

	// First-run UW initialization exactly as a new machine performs it.
	std::vector<uint8_t> initializedFlash;
	std::vector<uint8_t> factoryCache;
	{
		md::Hardware initializer(rom, _firmwarePath, md::MachineModel::Machinedrum);
		if(!initializer.isValid())
			return fail("firmware is not the supported Machinedrum OS 1.63 image");
		if(initializer.hasPlusDrive() || !initializer.copyPlusDriveData().empty())
			return fail("a new machine started with a +Drive connected");
		if(!initializeUwFlash(initializer))
			return fail("firmware did not complete first-run UW initialization");
		initializedFlash = initializer.copyFlashData();
		if(!md::encodeFactoryFlashCache(factoryCache, initializedFlash, rom))
			return fail("UW factory cache could not be encoded");
	}

	// OS 1.63 does not write its factory sample bank to UW flash when it sees a
	// +Drive during first-run initialization. The shared machine-local factory
	// cache must not depend on +Drive ownership, so the drive stays disconnected
	// until the reboot that follows initialization.
	{
		md::Hardware initializer(rom, _firmwarePath, md::MachineModel::Machinedrum);
		if(!initializer.installPlusDrive(blankDrive) || initializer.isPlusDriveConnected())
			return fail("first-run initialization connected the +Drive");
		if(!initializeUwFlash(initializer)
			|| drive(initializer).commandCount() != 0
			|| initializer.copyFlashData() != initializedFlash
			|| initializer.copyPlusDriveData() != blankDrive)
			return fail("+Drive ownership changed first-run UW factory initialization");
	}

	const auto cacheFile = baseLib::filesystem::validatePath(_home)
		+ "nvram/md-uw-1.63-factory-v2.cache";
	baseLib::filesystem::createDirectory(baseLib::filesystem::getPath(cacheFile));
	if(!baseLib::filesystem::writeFile(cacheFile, factoryCache))
		return fail("could not stage the isolated factory cache");

	// A Device without a +Drive keeps writing version-4 states.
	md::Device writerDevice(deviceParams(rom, _firmwarePath, _home));
	auto& writer = writerDevice.getHardware();
	std::vector<uint8_t> drivelessState;
	if(!writerDevice.getState(drivelessState, synthLib::StateTypeGlobal)
		|| drivelessState.size() < 6 || drivelessState[5] != 4)
		return fail("a Device without +Drive did not write a version-4 state");

	// Connect a blank +Drive. The OS initializes the serial MMC interface and
	// formats the blank drive; that format is a firmware write.
	if(!writer.installPlusDrive(blankDrive) || !writer.isPlusDriveConnected())
		return fail("blank +Drive could not be connected");
	const auto blankGeneration = writer.plusDriveGeneration();
	advance(writer, md::g_samplerate * 20);
	report("blank-drive boot", writer);
	if(!drive(writer).initialized())
		return fail("firmware did not initialize the +Drive interface");
	if(drive(writer).storedSectorCount() == 0
		|| writer.plusDriveGeneration() == blankGeneration || !writer.plusDriveDirty())
		return fail("firmware did not write (format) the blank +Drive");
	if(writer.copyFlashData() != initializedFlash)
		return fail("formatting the +Drive changed UW sample flash");
	const auto writtenDrive = writer.copyPlusDriveData();

	const auto encodeStart = std::chrono::steady_clock::now();
	std::vector<uint8_t> projectState;
	if(!writerDevice.getState(projectState, synthLib::StateTypeGlobal))
		return fail("project state could not be captured");
	const auto encodeMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - encodeStart).count();
	md::DecodedState decoded;
	if(!md::decodeState(decoded, projectState, rom, md::MachineModel::Machinedrum,
		synthLib::StateTypeGlobal) || !decoded.containsPlusDrive
		|| decoded.plusDrive != writtenDrive)
		return fail("project state did not carry the formatted +Drive");
	std::cout << "formatted +Drive: " << writtenDrive.size() << " image bytes, "
		<< projectState.size() << " project bytes, " << encodeMilliseconds
		<< " ms getState\n";
	// OS 1.63 formats a drive on a factory-initialized machine to ~6.2K sectors
	// (~3.2 MB of MDPD, ~4.1 MB of project state with patch RAM).
	if(projectState.size() > 8u * 1024u * 1024u)
		return fail("freshly formatted +Drive exceeded the 8 MiB project-state budget");

	// Restore the project into a fresh Device. The firmware must read the
	// formatted drive back and accept it instead of formatting again.
	md::Device restoredDevice(deviceParams(rom, _firmwarePath, _home));
	if(!restoredDevice.setState(projectState, synthLib::StateTypeGlobal)
		|| restoredDevice.hasDeferredStateRestore())
		return fail("project state with +Drive did not restore into a fresh Device");
	auto& restored = restoredDevice.getHardware();
	if(restored.copyPlusDriveData() != writtenDrive || restored.plusDriveDirty())
		return fail("restored Device did not install the project +Drive cleanly");
	const auto restoredGeneration = restored.plusDriveGeneration();
	advance(restored, md::g_samplerate * 20);
	report("restored boot", restored);
	if(!drive(restored).initialized() || drive(restored).commandCount() == 0)
		return fail("restored firmware did not read the +Drive");
	if(restored.plusDriveGeneration() != restoredGeneration
		|| restored.copyPlusDriveData() != writtenDrive)
		return fail("restored firmware rewrote a valid formatted +Drive");

	const auto mainScreen = restored.getFrontPanelSnapshot();
	if(!longPressChord(restored, md::PanelControl::Function,
		md::PanelControl::PatternSong))
		return fail("failed to open +Drive Snapshot selection");
	const auto snapshotScreen = restored.getFrontPanelSnapshot();
	if(!tap(restored, md::PanelControl::Exit)
		|| !longPressChord(restored, md::PanelControl::Function, md::PanelControl::Kit))
		return fail("failed to open +Drive sample-bank selection");
	const auto sampleBankScreen = restored.getFrontPanelSnapshot();
	if(sameLcd(mainScreen, snapshotScreen) || sameLcd(mainScreen, sampleBankScreen)
		|| sameLcd(snapshotScreen, sampleBankScreen))
		return fail("+Drive Snapshot and sample-bank selectors were not exposed");
	(void)tap(restored, md::PanelControl::Exit);

	// Optional migration fixture: an .mdpd checkpoint written by an earlier
	// build (for example alpha.7/alpha.8's md-plusdrive-standalone-v1.mdpd).
	// The firmware must accept it without reformatting. Never commit fixtures.
	if(const auto* const fixture = std::getenv("GEARMULATOR_MD_PLUSDRIVE_FIXTURE");
		fixture && *fixture)
	{
		std::vector<uint8_t> image;
		if(!baseLib::filesystem::readFile(image, fixture)
			|| !md::PlusDrive::validateStorage(image))
			return fail("the +Drive migration fixture is not a valid MDPD image");
		md::Device migratedDevice(deviceParams(rom, _firmwarePath, _home));
		if(!migratedDevice.installPlusDrive(image, false))
			return fail("the +Drive migration fixture could not be installed");
		auto& migrated = migratedDevice.getHardware();
		const auto generation = migrated.plusDriveGeneration();
		advance(migrated, md::g_samplerate * 20);
		report("migration fixture boot", migrated);
		const auto after = migrated.copyPlusDriveData();
		size_t changedRecords = 0;
		const size_t records = std::min(image.size(), after.size()) / 516;
		for(size_t i = 0; i < records; ++i)
			changedRecords += !std::equal(image.begin() + 16 + i * 516,
				image.begin() + 16 + (i + 1) * 516, after.begin() + 16 + i * 516);
		std::cout << "migration fixture: " << image.size() << " -> " << after.size()
			<< " bytes, " << changedRecords << " of " << records << " records changed, "
			<< (migrated.plusDriveGeneration() == generation ? "no writes" : "written") << '\n';
		// A rewritten bookkeeping sector is acceptable; a reformat is not.
		if(!drive(migrated).initialized() || after.size() != image.size()
			|| changedRecords * 10 > records)
			return fail("firmware reformatted the +Drive migration fixture");
	}

	// The two instances own independent drives.
	if(&restored == &writer || writer.copyPlusDriveData() != writtenDrive)
		return fail("restoring one Device changed another Device's +Drive");

	// Patch-only legacy states keep the live drive, like they keep the live
	// sample flash.
	auto legacyPatch = restored.copyPatchRam();
	legacyPatch[17] ^= 0x45;
	std::vector<uint8_t> legacyState;
	if(!md::encodeState(legacyState, legacyPatch, md::MachineModel::Machinedrum,
		synthLib::StateTypeGlobal)
		|| !restoredDevice.setState(legacyState, synthLib::StateTypeGlobal)
		|| restoredDevice.getHardware().copyPlusDriveData() != writtenDrive
		|| restoredDevice.getHardware().copyPatchRam() != legacyPatch)
		return fail("patch-only legacy state did not keep the live drive");

	// A complete UW state without a +Drive describes a drive-less machine. Loading
	// it must not inherit the previous project's drive.
	if(!restoredDevice.setState(drivelessState, synthLib::StateTypeGlobal)
		|| restoredDevice.getHardware().hasPlusDrive()
		|| restoredDevice.getHardware().isPlusDriveConnected())
		return fail("a drive-less project inherited the previous project's +Drive");

	// A machine without a local factory cache defers sparse UW state. The
	// isolated first-run candidate must not see the project +Drive, and the final
	// cold reboot must install it byte-exactly.
	md::Device deferredDevice(deviceParams(rom, _firmwarePath, {}));
	if(!deferredDevice.setState(projectState, synthLib::StateTypeGlobal)
		|| !deferredDevice.hasDeferredStateRestore())
		return fail("sparse +Drive project did not take the deferred restore path");
	auto* const candidate = md::DevicePreparedStateTestAccess::deferredHardware(
		deferredDevice);
	if(!candidate || candidate->isPlusDriveConnected())
		return fail("deferred first-run candidate connected the project +Drive");
	std::vector<uint8_t> stateDuringRestore;
	if(!deferredDevice.getState(stateDuringRestore, synthLib::StateTypeGlobal)
		|| stateDuringRestore != projectState)
		return fail("autosave during deferred restore lost the project +Drive");
	if(!initializeUwFlash(*candidate) || drive(*candidate).commandCount() != 0)
		return fail("deferred candidate did not initialize UW without its +Drive");
	uint64_t generation = 0;
	auto validated = deferredDevice.takeFinishedDeferredState(generation);
	if(!validated)
		return fail("deferred candidate did not finish");
	auto reboot = md::Device::makeDeferredStateReboot(*validated);
	if(!reboot || !deferredDevice.commitDeferredStateRestore(*reboot, generation))
		return fail("deferred +Drive project did not cold-boot");
	reboot.reset();
	validated.reset();
	if(deferredDevice.getHardware().copyPlusDriveData() != writtenDrive
		|| !deferredDevice.getHardware().isPlusDriveConnected())
		return fail("deferred restore did not install the project +Drive");
	std::vector<uint8_t> afterDeferred;
	if(!deferredDevice.getState(afterDeferred, synthLib::StateTypeGlobal)
		|| !md::decodeState(decoded, afterDeferred, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal) || decoded.plusDrive != writtenDrive)
		return fail("deferred restore state lost the project +Drive");

	std::cout << "mdPlusDriveFirmwareTest: PASS\n";
	return 0;
}

int main(const int argc, const char* const* argv)
{
	const char* const firmwarePath = argc == 2 ? argv[1]
		: std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	if(!firmwarePath || !*firmwarePath)
	{
		const auto* const required = std::getenv("GEARMULATOR_REQUIRE_FIRMWARE_TESTS");
		if(required && std::string(required) == "1")
			return fail("required MD firmware fixture is unavailable");
		std::cout << "mdPlusDriveFirmwareTest: SKIP (pinned MD firmware not supplied)\n";
		return 77;
	}
	// Keep the factory cache in an isolated home, never the user's data folder.
	const auto home = baseLib::filesystem::validatePath(
		baseLib::filesystem::getCurrentDirectory())
		+ "gearmulator-md-plusdrive-firmware-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()) + "/";
	baseLib::filesystem::createDirectory(home);
	const auto result = runFirmwareTest(firmwarePath, home);
	(void)baseLib::filesystem::remove(home + "nvram/md-uw-1.63-factory-v2.cache");
	(void)baseLib::filesystem::remove(home + "nvram");
	(void)baseLib::filesystem::remove(home);
	return result;
}
