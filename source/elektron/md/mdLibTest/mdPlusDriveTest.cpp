// +Drive backing-store and protocol tests, which need no firmware image: the
// backing file is machine-local and shared across OSes, the marker round-trips,
// malformed content is preserved as .bak, and FORMAT erase/tombstone behaviour
// is checked without destructive in-place recovery.
#include "mdLib/mdplusdrive.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
	static_assert(!std::is_copy_constructible_v<md::PlusDrive>);
	static_assert(!std::is_copy_assignable_v<md::PlusDrive>);
	static_assert(std::is_move_constructible_v<md::PlusDrive>);
	static_assert(std::is_move_assignable_v<md::PlusDrive>);

	int g_failures = 0;

	void check(const bool _cond, const char* _what)
	{
		if(!_cond)
		{
			std::fprintf(stderr, "FAIL %s\n", _what);
			++g_failures;
		}
	}

	void sendCommand(md::PlusDrive& _drive, const std::vector<uint8_t>& _bytes)
	{
		_drive.onPPDDRWrite(0x0c);
		for(const uint8_t byte : _bytes)
		{
			bool clockHigh = false;
			for(int bit = 7; bit >= 0; --bit)
			{
				const uint8_t data = (byte >> bit) & 1 ? 0x08 : 0x00;
				const uint8_t clock = clockHigh ? 0x04 : 0x00;
				_drive.onPPDATWrite(static_cast<uint8_t>(clock | data), 0x0c);
				clockHigh = !clockHigh;
			}
		}
		_drive.onPPDDRWrite(0x04);
	}

	uint8_t readNibble(md::PlusDrive& _drive, const uint32_t _pc = 0)
	{
		return static_cast<uint8_t>((_drive.dataBitForRead(0x04, _pc) >> 4) & 0x0f);
	}

	md::plusdrive::Page readMmPage(md::PlusDrive& _drive, const uint32_t _sector)
	{
		sendCommand(_drive, {
			0x52, static_cast<uint8_t>(_sector >> 24), static_cast<uint8_t>(_sector >> 16),
			static_cast<uint8_t>(_sector >> 8), static_cast<uint8_t>(_sector), 0x00});
		// MM 1.32b's internal-SRAM receiver polls before each 128-byte block,
		// then consumes eight data nibbles per longword. The poll is control
		// traffic and must not advance the flash cursor; there is no read-side
		// packet trailer in this path.
		md::plusdrive::Page page;
		for(uint32_t block = 0; block < 4; ++block)
		{
			(void)readNibble(_drive, 0x010000c2);
			for(uint32_t byte = 0; byte < 128; ++byte)
			{
				const uint8_t high = readNibble(_drive, 0x010000f2);
				const uint8_t low = readNibble(_drive, 0x010000f2);
				page[block * 128 + byte] = static_cast<uint8_t>((high << 4) | low);
			}
		}
		return page;
	}

	void writeMmPage(md::PlusDrive& _drive, const uint32_t _sector,
		const md::plusdrive::Page& _page)
	{
		if(_page.size() != md::plusdrive::g_sectorSize)
			return;
		sendCommand(_drive, {
			0x59, static_cast<uint8_t>(_sector >> 24), static_cast<uint8_t>(_sector >> 16),
			static_cast<uint8_t>(_sector >> 8), static_cast<uint8_t>(_sector), 0x00});
		_drive.onPPDDRWrite(0xf4);
		for(const uint8_t value : { uint8_t{0xf0}, uint8_t{0xf4}, uint8_t{0x00} })
			_drive.onPPDATWrite(value, 0xf4);
		for(const uint8_t byte : _page)
		{
			_drive.onPPDATWrite(static_cast<uint8_t>(byte & 0xf0), 0xf4);
			_drive.onPPDATWrite(static_cast<uint8_t>((byte << 4) & 0xf0), 0xf4);
		}
		_drive.onPPDATWrite(0xf4, 0xf4);
		_drive.onPPDDRWrite(0x04);
	}

	std::vector<uint8_t> fileBytes(const std::string& _path)
	{
		std::ifstream file(_path, std::ios::binary);
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
			std::istreambuf_iterator<char>());
	}

	std::string tempPath(const char* _name)
	{
		const char* dir = std::getenv("GEARMULATOR_TEST_TMPDIR");
		std::string base = (dir && *dir) ? dir : "/tmp";
		return base + "/md-plusdrive-test-" + _name + ".bin";
	}
}

int main()
{
	// Backing writes are coalesced on a wall-clock interval (a 1 MiB slot
	// save used to rewrite the whole image once per 512-byte phase); pin the
	// interval far out of reach here so the coalescing is deterministic and
	// the explicit flushBacking(true) calls below are what makes a file
	// durable. The product default is 500 ms (GEARMULATOR_PLUSDRIVE_FLUSHMS).
	setenv("GEARMULATOR_PLUSDRIVE_FLUSHMS", "600000", 1);

	// Filenames are machine-local (md vs mm) with no OS fingerprint:
	// the same file serves every OS on the machine (separate +Drive chip).
	const std::string md = md::plusdrive::filename("/home/", md::MachineModel::Machinedrum);
	const std::string mm = md::plusdrive::filename("/home/", md::MachineModel::Monomachine);
	check(md == "/home/nvram/md-plusdrive-v1.bin", "md filename");
	check(mm == "/home/nvram/mm-plusdrive-v1.bin", "mm filename");
	check(md::plusdrive::filename({}, md::MachineModel::Machinedrum).empty(), "empty home");

	// Sparse sector store round-trip: the wire address is a 512-byte sector
	// number, so only touched sectors are stored (a 1 GiB address space would
	// otherwise need a dense array).
	auto pageByte = [](const md::plusdrive::Pages& _pages,
		const uint64_t _byteAddr) -> uint8_t
	{
		const auto it = _pages.find(_byteAddr >> 9);
		return it == _pages.end() ? 0xff : it->second[_byteAddr & (md::plusdrive::g_sectorSize - 1)];
	};
	const std::string path = tempPath("roundtrip");
	std::remove(path.c_str());
	md::plusdrive::Pages pages;
	for(const uint32_t sector : { 0u, 1u, 0x100u, 0x1FFFFFu, 0x2FFFFFu })
	{
		md::plusdrive::Page& p = pages[sector];
		for(size_t i = 0; i < p.size(); ++i)
			p[i] = static_cast<uint8_t>((sector + i) & 0xff);
	}
	check(md::plusdrive::save(path, pages), "save");
	md::plusdrive::Pages loaded;
	check(md::plusdrive::load(loaded, path), "load");
	check(loaded == pages, "roundtrip pages");

	// ensureReady loads a valid file without touching it.
	md::plusdrive::Pages again;
	std::string error;
	check(md::plusdrive::ensureReady(again, path, &error), "ensureReady valid");
	check(again == pages, "ensureReady keeps bytes");

	// Unrecognized content is preserved as .bak; fresh erased image created.
	const std::string bad = tempPath("bad");
	{
		std::FILE* f = std::fopen(bad.c_str(), "wb");
		std::fwrite("junk-bytes-here!", 1, 16, f);
		std::fclose(f);
	}
	std::remove((bad + ".bak").c_str());
	md::plusdrive::Pages fresh;
	check(md::plusdrive::ensureReady(fresh, bad, &error), "ensureReady bad");
	check(fresh.empty(), "fresh erased");
	check(pageByte(fresh, 0) == 0xff && pageByte(fresh, 0x1FFFFF00u) == 0xff,
		"fresh reads erased");
	std::FILE* bak = std::fopen((bad + ".bak").c_str(), "rb");
	check(bak != nullptr, "bak preserved");
	if(bak)
		std::fclose(bak);
	md::plusdrive::Pages reloaded;
	check(md::plusdrive::load(reloaded, bad), "reload fresh");
	check(reloaded.empty(), "reloaded fresh empty");

	// A clean machine-local install has no +Drive image. Attaching the
	// responder must create the sparse store immediately, with an erased config
	// so the firmware can run its first-boot preparation pass.
	{
		const std::string firstPath = tempPath("first-boot-create");
		std::remove(firstPath.c_str());
		std::remove((firstPath + ".bak").c_str());
		md::PlusDrive firstDrive;
		std::string firstError;
		check(firstDrive.setBackingPath(firstPath, false, &firstError),
			"first-boot store opens from missing path");
		check(std::filesystem::exists(firstPath), "first-boot store is created");
		md::plusdrive::Pages firstPages;
		std::vector<uint8_t> firstConfig;
		check(md::plusdrive::load(firstPages, firstPath, &firstConfig, &firstError),
			"created first-boot store reloads");
		check(firstPages.empty(), "created first-boot store is initially empty");
		check(firstConfig.size() == md::plusdrive::g_configSize
			&& std::all_of(firstConfig.begin(), firstConfig.end(),
				[](const uint8_t _byte) { return _byte == 0xff; }),
			"created first-boot config is erased");
		std::remove(firstPath.c_str());
	}

	// Write persistence: a >6-byte transmit in one $0c phase persists the
	// trailing data bytes at the command address (bytes 1-4 big-endian).
	{
		md::PlusDrive drive;
		drive.setServeFlash(true);
		const std::string wpath = tempPath("write");
		std::remove(wpath.c_str());
		std::remove((wpath + ".bak").c_str());
		drive.setBackingPath(wpath);
		const uint8_t cmd[6] = { 0x42, 0x00, 0x00, 0x01, 0x00, 0x00 };
		const uint8_t data[4] = { 0xde, 0xad, 0xbe, 0xef };
		drive.onPPDDRWrite(0x0c);
		auto txByte = [&](const uint8_t _b)
		{
			for(int i = 7; i >= 0; --i)
			{
				const uint8_t bit = static_cast<uint8_t>((_b >> i) & 1);
				drive.onPPDATWrite(static_cast<uint8_t>(0x04 | (bit ? 0x08 : 0x00)), 0x0c);
			}
		};
		for(const auto b : cmd)
			txByte(b);
		for(const auto b : data)
			txByte(b);
		drive.onPPDDRWrite(0x04);
		check(drive.writePages() == 1, "one write page");
		// H0 round-trip: re-issue the command (read direction: 6-byte $0c,
		// flip to $04) and reconstruct the 4 bytes from served nibbles.
		drive.onPPDDRWrite(0x0c);
		for(const auto b : cmd)
			txByte(b);
		drive.onPPDDRWrite(0x04);
		uint8_t got[4] = {};
		for(int i = 0; i < 4; ++i)
		{
			const uint8_t hi = static_cast<uint8_t>((drive.dataBitForRead(0x04, 0) >> 4) & 0x0f);
			const uint8_t lo = static_cast<uint8_t>((drive.dataBitForRead(0x04, 0) >> 4) & 0x0f);
			got[i] = static_cast<uint8_t>((hi << 4) | lo);
		}
		check(got[0] == 0xde && got[1] == 0xad && got[2] == 0xbe && got[3] == 0xef,
			"H0 nibble round-trip");
		check(drive.backingSaves() == 0, "backing write coalesced");
		drive.flushBacking(true);
		check(drive.backingSaves() == 1, "forced flush writes once");
		md::plusdrive::Pages fromFile;
		check(md::plusdrive::load(fromFile, wpath), "write flushed to file");
		// The wire address is a 512-byte SECTOR number (the firmware shifts the
		// byte address right by 9, guarded by `$28d614`; see the geometry
		// entry), so command address 0x100 lands at byte offset 0x100 << 9.
		const uint32_t byteBase = 0x100u << 9;
		check(pageByte(fromFile, byteBase) == 0xde
			&& pageByte(fromFile, byteBase + 1) == 0xad
			&& pageByte(fromFile, byteBase + 2) == 0xbe
			&& pageByte(fromFile, byteBase + 3) == 0xef,
			"write bytes at sector address");
		check(pageByte(fromFile, byteBase - 1) == 0xff, "untouched byte erased");
		std::remove(wpath.c_str());
	}

	// The MD 16-KiB reader polls the bus before each 128-byte block. Those
	// polls are control reads and must not consume a flash nibble.
	{
		const std::string ppath = tempPath("52-block-poll");
		std::remove(ppath.c_str());
		std::remove((ppath + ".bak").c_str());
		md::plusdrive::Pages ppages;
		ppages[1][0] = 0xa5;
		ppages[1][1] = 0x5a;
		check(md::plusdrive::save(ppath, ppages), "52 poll fixture saves");
		md::PlusDrive drive;
		drive.setServeFlash(true);
		drive.setModel(md::MachineModel::Machinedrum);
		std::string error;
		check(drive.setBackingPath(ppath, false, &error), "52 poll store opens");
		sendCommand(drive, { 0x52, 0x00, 0x00, 0x00, 0x00, 0x00 });
		auto readAt = [&drive](const uint32_t pc)
		{
			return static_cast<uint8_t>((drive.dataBitForRead(0x04, pc) >> 4) & 0x0f);
		};
		check(readAt(0x0100099e) == 0, "52 initial status poll");
		for(uint32_t i = 0; i < 1024; ++i)
			(void)readAt(0x010009ce);
		check(readAt(0x0100099e) == 0, "52 block status poll");
		const uint8_t high = readAt(0x010009ce);
		const uint8_t low = readAt(0x010009ce);
		check(high == 0x0a && low == 0x05,
			"52 block poll does not consume flash nibble");
		std::remove(ppath.c_str());
		std::remove((ppath + ".bak").c_str());
	}

	// Config trailer round-trip: the config block written by
	// the 0x59 @ 0x1FFFFF flow persists next to the flash; legacy v1 files
	// (no trailer) load with an erased config so the first boot takes prep.
	{
		const std::string cpath = tempPath("config");
		std::remove(cpath.c_str());
		std::remove((cpath + ".bak").c_str());
		md::plusdrive::Pages fl;
		fl[0x1FFFFFu][0] = 0x11;
		std::vector<uint8_t> cfg(md::plusdrive::g_configSize, 0xff);
		cfg[0] = 0x4e; cfg[1] = 0x45; cfg[2] = 0x00; cfg[3] = 0x07;
		check(md::plusdrive::save(cpath, fl, cfg), "save with config");
		md::plusdrive::Pages lf;
		std::vector<uint8_t> lc;
		check(md::plusdrive::load(lf, cpath, &lc), "load with config");
		check(lc == cfg, "config round-trip");
		check(lf == fl, "flash round-trip with config");
		std::remove(cpath.c_str());
	}

	// Boot state-word read: the 0x52 @ 0x1FFFFF top probe
	// serves the persisted config word nibble-framed, so a prepared image
	// yields 0x4E450007 (no reboot) and an erased one 0xFFFFFFFF (prep).
	{
		md::PlusDrive drive;
		drive.setServeFlash(true);
		const std::string spath = tempPath("stateword");
		const std::string epath = tempPath("erased");
		std::remove(spath.c_str()); std::remove((spath + ".bak").c_str());
		std::remove(epath.c_str()); std::remove((epath + ".bak").c_str());
		md::plusdrive::Pages fl;
		std::vector<uint8_t> cfg(md::plusdrive::g_configSize, 0xff);
		cfg[0] = 0x4e; cfg[1] = 0x45; cfg[2] = 0x00; cfg[3] = 0x07;
		check(md::plusdrive::save(spath, fl, cfg), "save prepared");
		check(md::plusdrive::save(epath, fl, {}), "save erased");
		auto txByte = [&](const uint8_t _b)
		{
			for(int i = 7; i >= 0; --i)
			{
				const uint8_t bit = static_cast<uint8_t>((_b >> i) & 1);
				drive.onPPDATWrite(static_cast<uint8_t>(0x04 | (bit ? 0x08 : 0x00)), 0x0c);
			}
		};
		auto readWord = [&]() -> uint32_t
		{
			const uint8_t cmd[6] = { 0x52, 0x00, 0x1f, 0xff, 0xff, 0x00 };
			drive.onPPDDRWrite(0x0c);
			for(const auto b : cmd)
				txByte(b);
			drive.onPPDDRWrite(0x04);
			uint32_t v = 0;
			for(int i = 0; i < 9; ++i)
			{
				const uint8_t r = drive.dataBitForRead(0x04, 0);
				if(i > 0)
					v = (v << 4) | static_cast<uint8_t>((r >> 4) & 0x0f);
			}
			return v;
		};
		drive.setBackingPath(spath);
		check(readWord() == 0x4e450007u, "prepared state word served");
		drive.setBackingPath(epath);
		check(readWord() == 0xffffffffu, "erased state word served");
		std::remove(spath.c_str());
		std::remove(epath.c_str());
	}

	// Shared committed store + prepared copy-on-write overlay. Prepared writes
	// must be invisible until commit, and must not replace unrelated live data.
	{
		const std::string sharedPath = tempPath("shared-overlay");
		std::remove(sharedPath.c_str());
		std::remove((sharedPath + ".bak").c_str());
		md::PlusDrive live;
		live.setServeFlash(true);
		std::string error;
		check(live.setBackingPath(sharedPath, false, &error), "live store opens");
		sendCommand(live, { 0x42, 0x00, 0x00, 0x00, 0x01, 0x00, 0xa1 });
		check(live.writePages() == 1, "live overlay seed write");
		check(live.flushBacking(true), "live seed flush");

		md::PlusDrive prepared;
		prepared.setServeFlash(true);
		check(prepared.setBackingPath(sharedPath, true, &error), "prepared overlay opens");
		sendCommand(prepared, { 0x42, 0x00, 0x00, 0x00, 0x02, 0x00, 0xb2 });
		check(prepared.writePages() == 1, "prepared overlay write");
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x02, 0x00 });
		(void)readNibble(live);
		const uint8_t before[2] = { readNibble(live), readNibble(live) };
		check(before[0] == 0xf && before[1] == 0xf,
			"prepared writes stay isolated before commit");
		// A live write after the fork must produce a version conflict rather
		// than silently overwriting the live generation.
		sendCommand(live, { 0x42, 0x00, 0x00, 0x00, 0x03, 0x00, 0xc3 });
		check(!live.commitPrepared(prepared), "stale prepared overlay conflicts");
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x03, 0x00 });
		(void)readNibble(live);
		check(readNibble(live) == 0xc && readNibble(live) == 0x3,
			"live write survives stale prepared commit");
		// Re-fork from the current generation and commit successfully.
		md::PlusDrive retry;
		retry.setServeFlash(true);
		check(retry.setBackingPath(sharedPath, true, &error), "retry overlay opens");
		sendCommand(retry, { 0x42, 0x00, 0x00, 0x00, 0x02, 0x00, 0xb2 });
		check(live.commitPrepared(retry), "rebased prepared overlay commit");
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x02, 0x00 });
		(void)readNibble(live);
		const uint8_t after[2] = { readNibble(live), readNibble(live) };
		check(after[0] == 0xb && after[1] == 0x2,
			"prepared write visible after commit");
		md::plusdrive::Pages sharedLoaded;
		check(md::plusdrive::load(sharedLoaded, sharedPath), "shared overlay persisted");
		check(pageByte(sharedLoaded, 0x200) == 0xa1
			&& pageByte(sharedLoaded, 0x400) == 0xb2
			&& pageByte(sharedLoaded, 0x600) == 0xc3,
			"shared overlay preserves all committed sectors");

		const auto slash = sharedPath.find_last_of('/');
		const std::string aliasPath = sharedPath.substr(0, slash) + "/./"
			+ sharedPath.substr(slash + 1);
		md::PlusDrive alias;
		alias.setServeFlash(true);
		check(alias.setBackingPath(aliasPath, false, &error), "canonical path reuses store");
		// Populate live's page cache before the alias writes. A shared-store
		// generation change must invalidate that cache without a command boundary.
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x04, 0x00 });
		(void)readNibble(live);
		check(readNibble(live) == 0xf && readNibble(live) == 0xf,
			"uncached shared sector starts erased");
		sendCommand(alias, { 0x42, 0x00, 0x00, 0x00, 0x04, 0x00, 0xd4 });
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x04, 0x00 });
		(void)readNibble(live);
		check(readNibble(live) == 0xd && readNibble(live) == 0x4,
			"canonical path shares the same primary");
		check(alias.flushBacking(true), "canonical alias flushes");
		std::remove(sharedPath.c_str());
		std::remove((sharedPath + ".bak").c_str());
	}

	// A prepared write to an existing sector must copy the complete parent page
	// before changing one byte; unrelated bytes in that sector remain intact.
	{
		const std::string cowPath = tempPath("cow-same-sector");
		std::remove(cowPath.c_str());
		md::plusdrive::Pages seed;
		md::plusdrive::Page& seeded = seed[0x40];
		seeded.fill(0x5a);
		seeded[0] = 0x11;
		seeded[1] = 0x22;
		seeded[2] = 0x33;
		check(md::plusdrive::save(cowPath, seed), "same-sector COW fixture saves");
		md::PlusDrive live;
		md::PlusDrive prepared;
		live.setServeFlash(true);
		prepared.setServeFlash(true);
		std::string error;
		check(live.setBackingPath(cowPath, false, &error), "same-sector live store opens");
		check(prepared.setBackingPath(cowPath, true, &error), "same-sector overlay opens");
		sendCommand(prepared, { 0x42, 0x00, 0x00, 0x00, 0x40, 0x00, 0x77, 0x88 });
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x40, 0x00 });
		(void)readNibble(live);
		check(readNibble(live) == 0x1 && readNibble(live) == 0x1
			&& readNibble(live) == 0x2 && readNibble(live) == 0x2,
			"same-sector overlay stays private before commit");
		check(live.commitPrepared(prepared), "same-sector overlay commits");
		md::plusdrive::Pages loadedCow;
		check(md::plusdrive::load(loadedCow, cowPath)
			&& pageByte(loadedCow, uint64_t{0x40} << 9) == 0x77
			&& pageByte(loadedCow, (uint64_t{0x40} << 9) + 1) == 0x88
			&& pageByte(loadedCow, (uint64_t{0x40} << 9) + 2) == 0x33,
			"same-sector COW preserves untouched page bytes");
		std::remove(cowPath.c_str());
		std::remove((cowPath + ".bak").c_str());
	}

	// FORMAT's 0x63 erase transaction removes a complete sparse block, and
	// prepared overlays hide the parent until the erase is committed.
	{
		const std::string erasePath = tempPath("format-erase");
		std::remove(erasePath.c_str());
		md::plusdrive::Pages eraseSeed;
		eraseSeed[0x40].fill(0x3c);
		eraseSeed[0x440].fill(0x5e);
		check(md::plusdrive::save(erasePath, eraseSeed), "format erase fixture saves");
		md::PlusDrive erased;
		erased.setServeFlash(true);
		std::string error;
		check(erased.setBackingPath(erasePath, false, &error), "format erase store opens");
		sendCommand(erased, { 0x63, 0x00, 0x00, 0x00, 0x40, 0x00 });
		check(erased.flushBacking(true), "format erase flushes");
		md::plusdrive::Pages erasedLoaded;
		check(md::plusdrive::load(erasedLoaded, erasePath)
			&& erasedLoaded.find(0x40) == erasedLoaded.end()
			&& erasedLoaded.find(0x440) != erasedLoaded.end(),
			"format erase clears only its block");

		const std::string overlayPath = tempPath("format-erase-overlay");
		std::remove(overlayPath.c_str());
		auto overlaySeed = eraseSeed;
		overlaySeed[0x43f].fill(0x5e);
		check(md::plusdrive::save(overlayPath, overlaySeed), "format erase overlay fixture saves");
		md::PlusDrive parent;
		parent.setServeFlash(true);
		md::PlusDrive overlay;
		overlay.setServeFlash(true);
		check(parent.setBackingPath(overlayPath, false, &error), "format erase parent opens");
		check(overlay.setBackingPath(overlayPath, true, &error), "format erase overlay opens");
		sendCommand(overlay, { 0x63, 0x00, 0x00, 0x00, 0x40, 0x00 });
		sendCommand(overlay, { 0x52, 0x00, 0x00, 0x04, 0x3f, 0x00 });
		(void)readNibble(overlay);
		const uint8_t eraseReadA = readNibble(overlay);
		const uint8_t eraseReadB = readNibble(overlay);
		check(eraseReadA == 0xf && eraseReadB == 0xf,
			"format erase remains private in an overlay");
		check(parent.commitPrepared(overlay), "format erase overlay commits");
		md::plusdrive::Pages committedErase;
		check(md::plusdrive::load(committedErase, overlayPath)
			&& committedErase.find(0x43f) == committedErase.end(),
			"format erase tombstone commits over parent data");
		std::remove(erasePath.c_str());
		std::remove((erasePath + ".bak").c_str());
		std::remove(overlayPath.c_str());
		std::remove((overlayPath + ".bak").c_str());
	}

	// Overlay page caches observe a commit made through the parent endpoint.
	{
		const std::string tokenPath = tempPath("overlay-generation-cache");
		std::remove(tokenPath.c_str());
		md::plusdrive::Pages tokenSeed;
		tokenSeed[0x90].fill(0x11);
		check(md::plusdrive::save(tokenPath, tokenSeed), "overlay cache fixture saves");
		md::PlusDrive parent;
		md::PlusDrive overlay;
		parent.setServeFlash(true);
		overlay.setServeFlash(true);
		std::string error;
		check(parent.setBackingPath(tokenPath, false, &error), "overlay cache parent opens");
		check(overlay.setBackingPath(tokenPath, true, &error), "overlay cache view opens");
		sendCommand(overlay, { 0x52, 0x00, 0x00, 0x00, 0x90, 0x00 });
		(void)readNibble(overlay);
		check(readNibble(overlay) == 0x1 && readNibble(overlay) == 0x1,
			"overlay cache starts from parent data");
		sendCommand(parent, { 0x42, 0x00, 0x00, 0x00, 0x90, 0x00, 0x22 });
		check(parent.flushBacking(true), "overlay cache parent commits");
		sendCommand(overlay, { 0x52, 0x00, 0x00, 0x00, 0x90, 0x00 });
		(void)readNibble(overlay);
		check(readNibble(overlay) == 0x2 && readNibble(overlay) == 0x2,
			"overlay cache invalidates after parent commit");
		std::remove(tokenPath.c_str());
		std::remove((tokenPath + ".bak").c_str());
	}

	// Inline config writes use the same invariant as f4 writes: the top-sector
	// state word is captured by DriveStore rather than by one transport path.
	{
		const std::string inlineConfigPath = tempPath("inline-config");
		std::remove(inlineConfigPath.c_str());
		std::remove((inlineConfigPath + ".bak").c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(inlineConfigPath, false, &error), "inline config store opens");
		sendCommand(drive, { 0x59, 0x00, 0x1f, 0xff, 0xff, 0x00,
			0x4e, 0x45, 0x00, 0x07 });
		const auto config = drive.configBlock();
		check(config.size() == md::plusdrive::g_configSize
			&& config[0] == 0x4e && config[1] == 0x45
			&& config[2] == 0x00 && config[3] == 0x07,
			"inline config updates dedicated block");
		// A protocol reset is a persistence barrier even with the test's long
		// coalescing interval.
		drive.resetTransaction();
		check(drive.persistenceError().empty(), "inline config reset flush succeeds");
		md::plusdrive::Pages loaded;
		std::vector<uint8_t> loadedConfig;
		check(md::plusdrive::load(loaded, inlineConfigPath, &loadedConfig)
			&& loadedConfig.size() >= 4 && loadedConfig[0] == 0x4e
			&& loadedConfig[1] == 0x45 && loadedConfig[2] == 0x00 && loadedConfig[3] == 0x07,
			"inline config survives reload");
		std::remove(inlineConfigPath.c_str());
		std::remove((inlineConfigPath + ".bak").c_str());
	}

	// Address bounds: invalid wire sectors are complete no-ops and never alias
	// sector zero or create a file that load() must reject.
	{
		const std::string boundsPath = tempPath("bounds");
		std::remove(boundsPath.c_str());
		std::remove((boundsPath + ".bak").c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(boundsPath, false, &error), "bounds store opens");
		sendCommand(drive, { 0x42, 0x00, 0x40, 0x00, 0x00, 0x00, 0xde });
		check(drive.writePages() == 0, "out-of-range inline write ignored");
		sendCommand(drive, { 0x42, 0x00, 0x80, 0x00, 0x00, 0x00, 0xad });
		check(drive.writePages() == 0, "shift-wrap inline write ignored");
		check(drive.flushBacking(true), "bounds store remains valid");
		sendCommand(drive, { 0x52, 0x00, 0x80, 0x00, 0x00, 0x00 });
		(void)readNibble(drive);
		check(readNibble(drive) == 0xf && readNibble(drive) == 0xf,
			"invalid read is erased, not aliased");
		md::plusdrive::Pages badPages;
		badPages[md::plusdrive::g_sectorCount].fill(0x5a);
		check(!md::plusdrive::save(boundsPath, badPages),
			"out-of-range map key rejected by save");
		md::plusdrive::Pages validAfterBadSave;
		check(md::plusdrive::load(validAfterBadSave, boundsPath)
			&& validAfterBadSave.empty(), "rejected save leaves old image intact");
		std::remove(boundsPath.c_str());
		std::remove((boundsPath + ".bak").c_str());
	}

	// Unknown/partial commands fail closed rather than serving arbitrary flash.
	{
		const std::string unknownPath = tempPath("unknown-command");
		std::remove(unknownPath.c_str());
		md::plusdrive::Pages unknownPages;
		unknownPages[0x70].fill(0x5a);
		check(md::plusdrive::save(unknownPath, unknownPages), "unknown-command fixture saves");
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(unknownPath, false, &error), "unknown-command store opens");
		sendCommand(drive, { 0x55, 0x00, 0x00, 0x00, 0x70, 0x00 });
		(void)readNibble(drive);
		check(readNibble(drive) == 0xf && readNibble(drive) == 0xf,
			"unknown command does not expose flash data");
		std::remove(unknownPath.c_str());
		std::remove((unknownPath + ".bak").c_str());
	}

	// f4 bulk phase: two 1044-write phases store 512 bytes each, with the
	// trailer/dismiss writes excluded from storage.
	{
		const std::string bulkPath = tempPath("bulk-frame");
		std::remove(bulkPath.c_str());
		std::remove((bulkPath + ".bak").c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(bulkPath, false, &error), "bulk store opens");
		sendCommand(drive, { 0x59, 0x00, 0x00, 0x00, 0x20, 0x00 });
		auto phase = [&](const uint32_t _first)
		{
			drive.onPPDDRWrite(0xf4);
			for(const uint8_t value : { uint8_t{0xf0}, uint8_t{0xf4}, uint8_t{0x00} })
				drive.onPPDATWrite(value, 0xf4);
			for(uint32_t i = 0; i < 512; ++i)
			{
				const uint8_t byte = static_cast<uint8_t>(_first + i * 3);
				drive.onPPDATWrite(static_cast<uint8_t>(byte & 0xf0), 0xf4);
				drive.onPPDATWrite(static_cast<uint8_t>((byte << 4) & 0xf0), 0xf4);
			}
			drive.onPPDATWrite(0xf4, 0xf4);
			drive.onPPDDRWrite(0x04);
		};
		phase(0x11);
		phase(0x22);
		check(drive.flushBacking(true), "bulk phases flush");
		sendCommand(drive, { 0x52, 0x00, 0x00, 0x00, 0x20, 0x00 });
		(void)readNibble(drive);
		bool bytesOk = true;
		for(uint32_t i = 0; i < 1024; ++i)
		{
			const uint8_t byte = static_cast<uint8_t>((readNibble(drive) << 4)
				| readNibble(drive));
			const uint8_t expected = static_cast<uint8_t>((i < 512 ? 0x11 : 0x22)
				+ (i % 512) * 3);
			bytesOk = bytesOk && byte == expected;
		}
		check(bytesOk, "two f4 phases round-trip without trailer bytes");
		md::plusdrive::Pages bulkLoaded;
		check(md::plusdrive::load(bulkLoaded, bulkPath)
			&& pageByte(bulkLoaded, uint64_t{0x20} << 9) == 0x11
			&& pageByte(bulkLoaded, uint64_t{0x21} << 9) == 0x22,
			"f4 phases persist adjacent sectors");
		std::remove(bulkPath.c_str());
		std::remove((bulkPath + ".bak").c_str());
	}

	// A 16 KiB command is a hard upper bound. A 33rd phase must not wrap or
	// create a sector outside the selected 32-sector window.
	{
		const std::string boundedPath = tempPath("bulk-limit");
		std::remove(boundedPath.c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(boundedPath, false, &error), "bulk-limit store opens");
		sendCommand(drive, { 0x59, 0x00, 0x00, 0x00, 0x30, 0x00 });
		auto phase = [&](const uint8_t _fill)
		{
			drive.onPPDDRWrite(0xf4);
			for(const uint8_t value : { uint8_t{0xf0}, uint8_t{0xf4}, uint8_t{0x00} })
				drive.onPPDATWrite(value, 0xf4);
			for(int i = 0; i < 512; ++i)
			{
				drive.onPPDATWrite(static_cast<uint8_t>(_fill & 0xf0), 0xf4);
				drive.onPPDATWrite(static_cast<uint8_t>(_fill & 0xf0), 0xf4);
			}
			drive.onPPDATWrite(0xf4, 0xf4);
			drive.onPPDDRWrite(0x04);
		};
		for(uint8_t i = 0; i < 32; ++i)
			phase(i);
		phase(0xee); // rejected: exactly 32 sectors already consumed
		check(drive.flushBacking(true), "bulk-limit store flushes");
		md::plusdrive::Pages bounded;
		check(md::plusdrive::load(bounded, boundedPath)
			&& pageByte(bounded, uint64_t{0x4f} << 9) == 0x11
			&& bounded.find(0x50) == bounded.end(),
			"33rd f4 phase is bounded without sector wrap");
		std::remove(boundedPath.c_str());
		std::remove((boundedPath + ".bak").c_str());
	}

	// A stale 0x59 base must not make an unrelated 0x42 f4 phase writable.
	{
		const std::string stalePath = tempPath("stale-bulk-command");
		std::remove(stalePath.c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(stalePath, false, &error), "stale-bulk store opens");
		sendCommand(drive, { 0x59, 0x00, 0x00, 0x00, 0x60, 0x00 });
		sendCommand(drive, { 0x42, 0x00, 0x00, 0x00, 0x61, 0x00 });
		drive.onPPDDRWrite(0xf4);
		for(const uint8_t value : { uint8_t{0xf0}, uint8_t{0xf4}, uint8_t{0x00} })
			drive.onPPDATWrite(value, 0xf4);
		for(int i = 0; i < 32; ++i)
			drive.onPPDATWrite(0xab, 0xf4);
		drive.onPPDDRWrite(0x04);
		check(drive.flushBacking(true), "stale-bulk store flushes");
		md::plusdrive::Pages staleLoaded;
		check(md::plusdrive::load(staleLoaded, stalePath)
			&& staleLoaded.find(0x61) == staleLoaded.end(),
			"unrelated f4 phase cannot write a stale address");
		std::remove(stalePath.c_str());
		std::remove((stalePath + ".bak").c_str());
	}

	// MM 0x59 status profile used by SAVE. The first and second handshakes
	// expect the low/high/low bit-7 sequence; the existing 0x900 payload
	// remains sequenced on bit 3. Keep this firmware-profile check beside the
	// MM packet test so an MD-only status heuristic cannot silently
	// reintroduce the SAVE `ERROR` path.
	{
		md::PlusDrive drive;
		drive.setModel(md::MachineModel::Monomachine);
		sendCommand(drive, { 0x59, 0x00, 0x00, 0x00, 0x00, 0x00 });
		auto read = [&drive](const uint32_t _pc)
		{
			return drive.dataBitForRead(0x04, _pc);
		};
		check((read(0x217ed2) & 0x80) == 0, "MM save status pre-wait low");
		for(int i = 0; i < 3; ++i)
		{
			check((read(0x217f00) & 0x80) == 0, "MM save first status low");
			check((read(0x217f0a) & 0x80) != 0, "MM save middle status high");
		}
		check((read(0x217f18) & 0x80) == 0, "MM save third status low");
		// The status-data phase still carries the existing 0x900 sequence on
		// bit 3; the MM-specific fix here is the bit-7 handshake profile.
		bool sawStatusPayload = false;
		for(int i = 0; i < 32; ++i)
		{
			const uint8_t value = read((i & 1) ? 0x217f7a : 0x217f70);
			sawStatusPayload = sawStatusPayload || (value & 0x08) != 0;
		}
		check(sawStatusPayload, "MM save status payload remains sequenced");
		check((read(0x2180da) & 0x80) == 0, "MM save second pre-wait low");
		check((read(0x218108) & 0x80) == 0, "MM save second first status low");
		check((read(0x218118) & 0x80) != 0, "MM save second middle status high");
		check((read(0x218128) & 0x80) == 0, "MM save second third status low");
	}
	{
		md::PlusDrive drive;
		drive.setModel(md::MachineModel::Machinedrum);
		sendCommand(drive, { 0x59, 0x00, 0x00, 0x00, 0x00, 0x00 });
		check((drive.dataBitForRead(0x04, 0x240216) & 0x80) == 0,
			"MD save status pre-wait low");
		check((drive.dataBitForRead(0x04, 0x240244) & 0x80) == 0,
			"MD save first status low");
		check((drive.dataBitForRead(0x04, 0x240254) & 0x80) != 0,
			"MD save middle status high");
		check((drive.dataBitForRead(0x04, 0x240264) & 0x80) == 0,
			"MD save third status low");
	}

	// MM 1.32b 0x52 receivers use a continuous data stream. The two
	// firmware readers poll status before each 128-byte block; those polls
	// must not consume a nibble, and no 8-byte read trailer is inserted at a
	// 512-byte boundary. Walk a 32-sector command and the following command
	// so both the block and command boundaries are covered.
	{
		const std::string mmPath = tempPath("mm-continuous");
		std::remove(mmPath.c_str());
		std::remove((mmPath + ".bak").c_str());
		md::plusdrive::Pages pages;
		pages[0][0] = 0x12;
		pages[0][1] = 0x34;
		pages[1][0] = 0x5a;
		pages[1][1] = 0xa5;
		pages[31][0] = 0x31;
		pages[32][0] = 0x20;
		pages[32][1] = 0x32;
		pages[33][0] = 0x33;
		pages[0x200000][0] = 0x5c;
		pages[0x200000][1] = 0xa5;
		check(md::plusdrive::save(mmPath, pages), "MM continuous fixture saves");
		md::PlusDrive drive;
		drive.setServeFlash(true);
		drive.setModel(md::MachineModel::Monomachine);
		std::string error;
		check(drive.setBackingPath(mmPath, false, &error), "MM continuous store opens");

		auto readCommand = [&](const uint32_t _base, const uint32_t _sectors,
			const uint32_t _pollPc, const uint32_t _dataPc)
		{
			sendCommand(drive, { 0x52,
				static_cast<uint8_t>(_base >> 24), static_cast<uint8_t>(_base >> 16),
				static_cast<uint8_t>(_base >> 8), static_cast<uint8_t>(_base), 0x00 });
			for(uint32_t sector = 0; sector < _sectors; ++sector)
			{
				for(uint32_t block = 0; block < 4; ++block)
				{
					// The first poll is also the command's initial upper-bus
					// poll; later polls must be suppressed at their firmware PC.
					(void)readNibble(drive, _pollPc);
					for(uint32_t byte = 0; byte < 128; ++byte)
					{
						const uint8_t high = readNibble(drive, _dataPc);
						const uint8_t low = readNibble(drive, _dataPc);
						const uint32_t absoluteSector = _base + sector;
						const size_t absoluteByte = sector * 512 + block * 128 + byte;
						if(byte == 0 && (absoluteSector == 0 || absoluteSector == 1
							|| absoluteSector == 31 || absoluteSector == 32 || absoluteSector == 33))
						{
							const auto& expected = pages[absoluteSector];
							check((high << 4 | low) == expected[absoluteByte % 512],
								"MM continuous stream preserves sector markers");
						}
					}
				}
			}
		};
		readCommand(0, 32, 0x010000c2, 0x010000f2);
		readCommand(0x20, 2, 0x010000c2, 0x010000f2);
		// The ROM-resident MM receiver has a second status-poll window. Its
		// data cursor must remain aligned there as well.
		readCommand(0x200000, 1, 0x0216da8, 0x0216dd2);
		std::remove(mmPath.c_str());
		std::remove((mmPath + ".bak").c_str());
	}

	// A deterministic MM bank workflow is also a storage acceptance test:
	// copy a complete bank-A page set into bank B, switch/read bank B, change
	// its lock record, erase the bank, and reconstruct a fresh endpoint. The
	// checks intentionally compare complete 512-byte pages, not only a menu
	// counter or a file hash, so an omitted page cannot hide behind a sector
	// that happens to contain a matching marker.
	{
		constexpr uint32_t bankA = 0x200000;
		// MM banks are 0x400 sectors (512 KiB) each; the waveform slots
		// within a bank are 0x10 sectors apart and the directory is the last
		// sector of the bank window.
		constexpr uint32_t bankB = 0x200400;
		constexpr uint32_t directoryOffset = 480;
		constexpr uint32_t pageCount = 4;
		const std::string workflowPath = tempPath("mm-bank-cold-workflow");
		std::remove(workflowPath.c_str());
		std::remove((workflowPath + ".bak").c_str());

		auto makePage = [](const uint8_t _seed, const uint32_t _index)
		{
			md::plusdrive::Page page;
			for(size_t i = 0; i < page.size(); ++i)
				page[i] = static_cast<uint8_t>(_seed + i * 3 + _index);
			page[0] = _seed;
			page[1] = static_cast<uint8_t>(_index);
			page[2] = 0xa5;
			page[3] = 0x5a;
			return page;
		};
		auto makeDirectory = [&](const char* const _name, const uint8_t _index,
			const uint8_t _lock)
		{
			md::plusdrive::Page page;
			page.fill(0xff);
			std::memcpy(page.data() + directoryOffset, _name, std::strlen(_name));
			page[directoryOffset + 7] = 0xc7;
			page[directoryOffset + 8] = 0xa8;
			page[directoryOffset + 9] = 0x40;
			page[directoryOffset + 10] = 0x70;
			page[directoryOffset + 11] = 0x20;
			page[directoryOffset + 12] = 0x00;
			page[directoryOffset + 13] = _lock;
			page[directoryOffset + 14] = 0xab;
			page[directoryOffset + 15] = _index;
			return page;
		};

		md::plusdrive::Pages seed;
		for(uint32_t i = 0; i < pageCount; ++i)
		{
			seed[bankA + i] = makePage(0x31, i);
			seed[bankB + i] = makePage(0x72, i);
		}
		seed[bankA + 0x3ff] = makeDirectory("BANK1", 0, 0);
		seed[bankB + 0x3ff] = makeDirectory("BANK2", 1, 0);
		seed[0x12345] = makePage(0xc3, 9);
		std::vector<uint8_t> config(md::plusdrive::g_configSize, 0xff);
		config[0] = 0x4e; config[1] = 0x45; config[2] = 0x03; config[3] = 0x00;
		check(md::plusdrive::save(workflowPath, seed, config),
			"MM bank workflow fixture saves");

		md::plusdrive::Pages expected = seed;
		{
			md::PlusDrive live;
			live.setServeFlash(true);
			live.setModel(md::MachineModel::Monomachine);
			std::string error;
			check(live.setBackingPath(workflowPath, false, &error),
				"MM bank workflow store opens");
			for(uint32_t i = 0; i < pageCount; ++i)
			{
				const auto source = readMmPage(live, bankA + i);
				const auto target = readMmPage(live, bankB + i);
				check(source == seed[bankA + i], "MM bank A page reads before copy");
				check(target == seed[bankB + i], "MM bank B page reads before copy");
				writeMmPage(live, bankB + i, source);
				expected[bankB + i] = source;
			}
			// The manager's lock bit lives in the bank directory record. Keep
			// the name/valid marker intact and change only that byte.
			auto directory = readMmPage(live, bankB + 0x3ff);
			directory[directoryOffset + 13] = 1;
			writeMmPage(live, bankB + 0x3ff, directory);
			expected[bankB + 0x3ff] = directory;
			check(live.flushBacking(true), "MM bank copy/lock flushes");
		}

		md::plusdrive::Pages copied;
		std::vector<uint8_t> copiedConfig;
		check(md::plusdrive::load(copied, workflowPath, &copiedConfig),
			"MM bank workflow reloads after copy");
		check(copied == expected && copiedConfig == config,
			"MM bank copy/lock page set and config are exact");
		{
			md::PlusDrive cold;
			cold.setServeFlash(true);
			cold.setModel(md::MachineModel::Monomachine);
			std::string error;
			check(cold.setBackingPath(workflowPath, false, &error),
				"MM bank cold endpoint opens");
			for(uint32_t i = 0; i < pageCount; ++i)
				check(readMmPage(cold, bankB + i) == expected[bankB + i],
					"MM bank B survives cold endpoint reconstruction");
			const auto directory = readMmPage(cold, bankB + 0x3ff);
			check(directory == expected[bankB + 0x3ff]
				&& directory[directoryOffset + 13] == 1,
				"MM bank lock survives cold endpoint reconstruction");
			auto unlocked = directory;
			unlocked[directoryOffset + 13] = 0;
			writeMmPage(cold, bankB + 0x3ff, unlocked);
			expected[bankB + 0x3ff] = unlocked;
			check(cold.flushBacking(true), "MM bank unlock flushes");
			check(readMmPage(cold, bankB + 0x3ff) == unlocked,
				"MM bank unlock is durable on the same endpoint");
			{
				md::PlusDrive unlockCold;
				unlockCold.setServeFlash(true);
				unlockCold.setModel(md::MachineModel::Monomachine);
				std::string unlockError;
				check(unlockCold.setBackingPath(workflowPath, false, &unlockError),
					"MM bank unlock cold endpoint opens");
				check(readMmPage(unlockCold, bankB + 0x3ff) == unlocked,
					"MM bank unlock survives cold endpoint reconstruction");
			}

			// Model the two payload phases of a real cross-bank paste/clear
			// with a deterministic marker. This is intentionally a page-level
			// storage test: the menu harness supplies the firmware UI proof.
			auto pasted = readMmPage(cold, bankB + 1);
			pasted[4] = 0xc3;
			writeMmPage(cold, bankB + 1, pasted);
			expected[bankB + 1] = pasted;
			check(cold.flushBacking(true), "MM bank paste marker flushes");
			check(readMmPage(cold, bankB + 1) == pasted
				&& readMmPage(cold, bankA + 1) == seed[bankA + 1],
				"MM bank paste changes only destination page");
			auto cleared = pasted;
			cleared[4] = 0xff;
			writeMmPage(cold, bankB + 1, cleared);
			expected[bankB + 1] = cleared;
			check(cold.flushBacking(true), "MM bank clear marker flushes");
			check(readMmPage(cold, bankB + 1) == cleared
				&& readMmPage(cold, bankA + 1) == seed[bankA + 1],
				"MM bank clear preserves source page");

			// Erase only bank B's 0x400-sector window, then clear its directory
			// sector as the firmware's FORMAT transaction does.
			sendCommand(cold, { 0x63, 0x00, 0x20, 0x04, 0x00, 0x00 });
			sendCommand(cold, { 0x64, 0x00, 0x20, 0x07, 0xff, 0x00 });
			check(cold.flushBacking(true), "MM bank erase flushes");
		}

		md::plusdrive::Pages erased;
		std::vector<uint8_t> erasedConfig;
		check(md::plusdrive::load(erased, workflowPath, &erasedConfig),
			"MM bank erased store reloads");
		check(erasedConfig == config, "MM bank erase preserves config trailer");
		check(erased[bankA] == seed[bankA], "MM bank erase preserves bank A");
		bool bankBGone = true;
		for(uint32_t i = 0; i < pageCount; ++i)
			bankBGone = bankBGone && erased.find(bankB + i) == erased.end();
		bankBGone = bankBGone && erased.find(bankB + 0x3ff) == erased.end();
		check(bankBGone, "MM bank erase removes bank B pages and directory");
		{
			md::PlusDrive cold;
			cold.setServeFlash(true);
			cold.setModel(md::MachineModel::Monomachine);
			std::string error;
			check(cold.setBackingPath(workflowPath, false, &error),
				"MM bank erased cold endpoint opens");
			check(readMmPage(cold, bankA) == seed[bankA],
				"MM bank A remains readable after cold erase");
			md::plusdrive::Page erasedPage;
			erasedPage.fill(0xff);
			check(readMmPage(cold, bankB) == erasedPage,
				"MM erased bank B reads erased after cold reconstruction");
		}
		check(erased[0x12345] == seed[0x12345],
			"MM bank erase preserves unrelated sparse page");
		std::remove(workflowPath.c_str());
		std::remove((workflowPath + ".bak").c_str());
	}

	// MM Digibank payload writes use the same bounded f4 path at the high
	// 0x200000 data region, independent of the MM packet-read trailer.
	{
		const std::string contentPath = tempPath("mm-digibank-content");
		std::remove(contentPath.c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		drive.setModel(md::MachineModel::Monomachine);
		std::string error;
		check(drive.setBackingPath(contentPath, false, &error), "MM content store opens");
		sendCommand(drive, { 0x59, 0x00, 0x20, 0x08, 0x00, 0x00 });
		drive.onPPDDRWrite(0xf4);
		for(const uint8_t value : { uint8_t{0xf0}, uint8_t{0xf4}, uint8_t{0x00} })
			drive.onPPDATWrite(value, 0xf4);
		for(uint32_t i = 0; i < 512; ++i)
		{
			const uint8_t value = static_cast<uint8_t>(0xa0 + (i & 0x0f));
			drive.onPPDATWrite(static_cast<uint8_t>(value & 0xf0), 0xf4);
			drive.onPPDATWrite(static_cast<uint8_t>((value << 4) & 0xf0), 0xf4);
		}
		drive.onPPDATWrite(0xf4, 0xf4);
		drive.onPPDDRWrite(0x04);
		check(drive.flushBacking(true), "MM content phase flushes");
		md::plusdrive::Pages loaded;
		check(md::plusdrive::load(loaded, contentPath)
			&& pageByte(loaded, uint64_t{0x200800} << 9) == 0xa0
			&& pageByte(loaded, (uint64_t{0x200800} << 9) + 1) == 0xa1,
			"MM Digibank data page persists at its sector address");
		std::remove(contentPath.c_str());
		std::remove((contentPath + ".bak").c_str());
	}

	// MM FORMAT erases the high Digibank block and its directory entry, not
	// merely the low snapshot region used by the MD fixture above.
	{
		const std::string erasePath = tempPath("mm-format-erase");
		std::remove(erasePath.c_str());
		md::plusdrive::Pages eraseSeed;
		eraseSeed[0x200000].fill(0x31);
		eraseSeed[0x200200].fill(0x32);
		eraseSeed[0x2003ff].fill(0x33);
		check(md::plusdrive::save(erasePath, eraseSeed), "MM format erase fixture saves");
		md::PlusDrive erased;
		erased.setServeFlash(true);
		erased.setModel(md::MachineModel::Monomachine);
		std::string error;
		check(erased.setBackingPath(erasePath, false, &error), "MM format erase store opens");
		sendCommand(erased, { 0x63, 0x00, 0x20, 0x00, 0x00, 0x00 });
		sendCommand(erased, { 0x64, 0x00, 0x20, 0x03, 0xff, 0x00 });
		check(erased.flushBacking(true), "MM format erase flushes");
		md::plusdrive::Pages erasedLoaded;
		const bool erasedOk = md::plusdrive::load(erasedLoaded, erasePath)
			&& erasedLoaded.find(0x200000) == erasedLoaded.end()
			&& erasedLoaded.find(0x200200) == erasedLoaded.end()
			&& erasedLoaded.find(0x2003ff) == erasedLoaded.end();
		if(!erasedOk)
		{
			std::fprintf(stderr, "MM format erase keys:");
			for(const auto& entry : erasedLoaded)
				std::fprintf(stderr, " %06x", entry.first);
			std::fprintf(stderr, "\n");
		}
		check(erasedOk, "MM format erase clears high data and directory");
		std::remove(erasePath.c_str());
		std::remove((erasePath + ".bak").c_str());
	}

	// Parent creation and malformed-input transactionality.
	{
		const std::string nestedRoot = tempPath("nested-root");
		const std::string nestedPath = nestedRoot + "/a/b/store.bin";
		std::filesystem::remove_all(nestedRoot);
		md::PlusDrive nested;
		std::string error;
		check(nested.setBackingPath(nestedPath, false, &error)
			&& nested.flushBacking(true), "nested backing parent is created");
		md::plusdrive::Pages nestedPages;
		check(md::plusdrive::load(nestedPages, nestedPath),
			"nested backing file loads");
		std::filesystem::remove_all(nestedRoot);
	}
	{
		const std::string blocker = tempPath("parent-blocker");
		const std::string badPath = blocker + "/drive.bin";
		std::remove(blocker.c_str());
		std::FILE* blockerFile = std::fopen(blocker.c_str(), "wb");
		check(blockerFile != nullptr, "parent blocker created");
		if(blockerFile)
		{
			std::fputs("not-a-directory", blockerFile);
			std::fclose(blockerFile);
		}
		md::PlusDrive failed;
		std::string error;
		check(!failed.setBackingPath(badPath, false, &error) && !error.empty(),
			"unwritable parent reports failure");
		std::remove(blocker.c_str());
	}
	{
		const std::string failurePath = tempPath("flush-failure");
		std::remove(failurePath.c_str());
		std::remove((failurePath + ".bak").c_str());
		md::PlusDrive drive;
		drive.setServeFlash(true);
		std::string error;
		check(drive.setBackingPath(failurePath, false, &error), "flush-failure store opens");
		sendCommand(drive, { 0x42, 0x00, 0x00, 0x00, 0x09, 0x00, 0x5a });
		std::error_code ec;
		std::filesystem::remove(failurePath, ec);
		ec.clear();
		check(std::filesystem::create_directory(failurePath, ec) && !ec,
			"flush-failure obstruction created");
		const auto savesBefore = drive.backingSaves();
		check(!drive.flushBacking(true), "atomic flush failure is reported");
		check(!drive.persistenceError().empty(), "flush failure exposes error");
		check(drive.backingSaves() == savesBefore, "failed flush is not counted");
		ec.clear();
		check(std::filesystem::remove(failurePath, ec) && !ec,
			"flush-failure obstruction removed");
		check(drive.flushBacking(true) && drive.persistenceError().empty(),
			"dirty snapshot retries successfully");
		md::plusdrive::Pages recovered;
		check(md::plusdrive::load(recovered, failurePath)
			&& pageByte(recovered, uint64_t{0x09} << 9) == 0x5a,
			"retried snapshot is durable");
		std::remove(failurePath.c_str());
	}
	{
		const std::string commitFailurePath = tempPath("commit-flush-failure");
		std::remove(commitFailurePath.c_str());
		std::remove((commitFailurePath + ".bak").c_str());
		md::PlusDrive live;
		md::PlusDrive prepared;
		live.setServeFlash(true);
		prepared.setServeFlash(true);
		std::string error;
		check(live.setBackingPath(commitFailurePath, false, &error),
			"commit-failure live store opens");
		sendCommand(live, { 0x42, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x11 });
		check(live.flushBacking(true), "commit-failure seed flushes");
		check(prepared.setBackingPath(commitFailurePath, true, &error),
			"commit-failure overlay opens");
		sendCommand(prepared, { 0x42, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x5a });

		// Make the forced commit save fail after the overlay has been merged.
		std::error_code ec;
		std::filesystem::remove(commitFailurePath, ec);
		ec.clear();
		check(std::filesystem::create_directory(commitFailurePath, ec) && !ec,
			"commit-failure obstruction created");
		const uint64_t generationBefore = live.storageGeneration();
		check(!live.commitPrepared(prepared, &error),
			"failed prepared commit is rejected");
		check(!error.empty(), "failed prepared commit exposes persistence error");
		check(live.storageGeneration() == generationBefore,
			"failed prepared commit rolls back generation");
		sendCommand(live, { 0x52, 0x00, 0x00, 0x00, 0x0b, 0x00 });
		(void)readNibble(live);
		check(readNibble(live) == 0xf && readNibble(live) == 0xf,
			"failed prepared commit does not leak overlay bytes");

		ec.clear();
		check(std::filesystem::remove(commitFailurePath, ec) && !ec,
			"commit-failure obstruction removed");
		check(live.commitPrepared(prepared, &error) && error.empty(),
			"prepared commit retries after persistence recovers");
		md::plusdrive::Pages committed;
		check(md::plusdrive::load(committed, commitFailurePath)
			&& pageByte(committed, uint64_t{0x0b} << 9) == 0x5a,
			"retried prepared commit is durable");
		std::remove(commitFailurePath.c_str());
		std::remove((commitFailurePath + ".bak").c_str());
	}
	{
		const std::string future = tempPath("future-version");
		std::remove(future.c_str());
		std::remove((future + ".bak").c_str());
		std::vector<uint8_t> header(16, 0);
		const char magic[] = "MDPLUSDRV1";
		std::memcpy(header.data(), magic, 10);
		header[10] = 0;
		header[11] = 3;
		{
			std::ofstream file(future, std::ios::binary);
			file.write(reinterpret_cast<const char*>(header.data()), header.size());
		}
		const auto before = fileBytes(future);
		md::plusdrive::Pages ignored;
		check(!md::plusdrive::ensureReady(ignored, future, &error),
			"future format is rejected");
		check(fileBytes(future) == before && !baseLib::filesystem::exists(future + ".bak"),
			"future format is preserved without downgrade");
		std::remove(future.c_str());
	}
	{
		const std::string conflict = tempPath("backup-conflict");
		const std::string backup = conflict + ".bak";
		std::remove(conflict.c_str());
		std::remove(backup.c_str());
		{
			std::ofstream file(conflict, std::ios::binary);
			const char invalid[] = "not-a-plusdrive-file";
			file.write(invalid, sizeof(invalid) - 1);
		}
		md::plusdrive::Pages backupPages;
		backupPages[7][0] = 0x5a;
		check(md::plusdrive::save(backup, backupPages), "conflict backup fixture");
		const auto primaryBefore = fileBytes(conflict);
		const auto backupBefore = fileBytes(backup);
		md::plusdrive::Pages ignored;
		check(!md::plusdrive::ensureReady(ignored, conflict, &error),
			"existing backup blocks destructive recovery");
		check(fileBytes(conflict) == primaryBefore && fileBytes(backup) == backupBefore,
			"conflicting recovery leaves both files untouched");
		std::remove(conflict.c_str());
		std::remove(backup.c_str());
	}
	{
		const std::string recovery = tempPath("backup-recovery");
		const std::string backup = recovery + ".bak";
		std::remove(recovery.c_str());
		std::remove(backup.c_str());
		md::plusdrive::Pages recovered;
		recovered[0x123][4] = 0x6b;
		std::vector<uint8_t> recoveredConfig(md::plusdrive::g_configSize, 0xa5);
		check(md::plusdrive::save(backup, recovered, recoveredConfig),
			"recovery backup fixture");
		md::plusdrive::Pages loaded;
		std::vector<uint8_t> loadedConfig;
		check(md::plusdrive::ensureReady(loaded, recovery, &error, &loadedConfig),
			"missing primary recovers from valid backup");
		check(pageByte(loaded, uint64_t{0x123} << 9 | 4) == 0x6b
			&& loadedConfig == recoveredConfig && baseLib::filesystem::exists(backup),
			"recovered primary and backup are both valid");
		std::remove(recovery.c_str());
		std::remove(backup.c_str());
	}
	{
		const std::string malformed = tempPath("malformed");
		std::remove(malformed.c_str());
		std::vector<uint8_t> header(16, 0);
		const char magic[] = "MDPLUSDRV1";
		std::memcpy(header.data(), magic, 10);
		header[10] = 0;
		header[11] = 2;
		header[12] = 0xff;
		header[13] = 0xff;
		header[14] = 0xff;
		header[15] = 0xff;
		std::ofstream malformedFile(malformed, std::ios::binary);
		malformedFile.write(reinterpret_cast<const char*>(header.data()), header.size());
		malformedFile.close();
		md::plusdrive::Pages sentinel;
		sentinel[7].fill(0x5a);
		std::vector<uint8_t> config(16, 0x3c);
		check(!md::plusdrive::load(sentinel, malformed, &config),
			"malformed file rejected");
		check(sentinel.size() == 1 && sentinel[7][0] == 0x5a
			&& config.size() == 16 && config[0] == 0x3c,
			"failed load leaves outputs transactional");
		std::remove(malformed.c_str());
	}

	std::remove(path.c_str());
	std::remove(bad.c_str());
	std::remove((bad + ".bak").c_str());

	if(g_failures == 0)
		std::printf("mdPlusDriveTest ok\n");
	return g_failures == 0 ? 0 : 1;
}
