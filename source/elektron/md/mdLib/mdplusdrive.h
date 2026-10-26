#pragma once

#include "mdtypes.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace md
{
	class Sim;
	class Device;
	class DriveStore;

	namespace plusdrive
	{
		// The firmware addresses the drive in 512-byte sectors, not bytes:
		// 0x23ed20/0x23f04c (0x52 read), 0x23fdf0 (0x59 write) and 0x23f2aa
		// (0x63) shift the byte address right by 9 while $28d614 is set, and
		// the boot state-word reader at 0x2405be passes 0x3FFFFE00 for the top
		// of the drive (0x3FFFFE00 >> 9 == 0x1FFFFF). Storage is sparse and
		// keyed by the full sector number, so the data region at the bottom of
		// the space and the metadata near the top cannot alias.
		static constexpr uint32_t g_sectorSize = 512;
		// Observed wire sectors reach 0x2FFFFF (FORMAT writes 0x2FDxxx..0x2FFFFF),
		// so bound the space at 2^22 sectors (2 GiB).
		static constexpr uint32_t g_sectorCount = 1u << 22;
		using Page = std::array<uint8_t, g_sectorSize>;
		using Pages = std::map<uint32_t, Page>;
	}

	// Virtual Machinedrum/Monomachine +Drive: a flash chip on ColdFire Port A
	// behind XOR gates (MAME elektronmono.cpp: bit 2 clock, bit 3 bidirectional
	// data). This answers the firmware's Port A bit-bang protocol so detection
	// succeeds and the GLOBAL > FILE > +DRIVE menu appears.
	//
	// Measured electrical contract:
	//   * One data bit per PPDAT write; bit2 is the clock phase, bit3 the data
	//     line.
	//   * 16-bit words move through the firmware's nibble transport.
	//   * Transmit: PPDDR = $0c (MCU drives clock+data).
	//   * Receive: PPDDR = $04 (MCU drives clock; device drives data).
	//   * Boot probe: init pair $0001/$19e0, then a 3-word command
	//     ($1980 $800f $0f19) repeated up to 5000x, each answered by a read
	//     window. No responder => all-1s reads => the menu stays hidden.
	//
	// The frame protocol is observed; command semantics come from the driver
	// disassembly, validated against real firmware boots.
	class PlusDrive
	{
	public:
		// Returned in the driver's boot read window. Chosen so the driver's
		// post-read checks accept the device; verified against firmware.
		static constexpr uint16_t g_statusWord = 0x0001;

		// Respond-and-observe modes. The boot probe at 0x23fc58 polls
		// send_raw(0x41, 0x40ff8000) up to 10000x for magic returns 0x80FF8000
		// (masked) / 0xC0FF8080 (full); absent hardware (idle-high bit3) makes
		// send_raw return -1. Disassembly: send_raw (0x23f3f4) polls bit3 64x
		// then shifts 7 bits into D6 (0x23f5c0); D6&$c0==0 enters the 32-bit
		// read path (0x23f62e), which returns D5. Both shift loops use sne
		// ($FF/00) + asl, so the response maps MSB-first to the return value.
		enum class Mode : uint8_t
		{
			AlwaysLow = 0,	// drive bit3 low in every receive window (enter Path B)
			AlwaysHigh = 1,	// drive high = absent behaviour (baseline)
			Canned = 2,		// 7x0 preamble + 0xC0FF8080 MSB-first, repeating
		};

		// Bit-order variants for magic brute-force. The driver's shift assembly
		// (sne+asl) is MSB-first per our model; byte order and LSB/MSB are
		// confirmed by experiment against flag 0x28d614.
		enum class CannedOrder : uint8_t
		{
			MsbNormal = 0,	// MSB-first, bytes C0 FF 80 80
			LsbNormal = 1,	// LSB-first per byte, bytes C0 FF 80 80
			MsbReversed = 2,	// MSB-first, bytes 80 80 FF C0
			LsbReversed = 3,	// LSB-first, bytes 80 80 FF C0
		};

		PlusDrive();
		// Durable storage is shared by replacement machines, protocol state is
		// not, so copying a whole store is both expensive and unsafe at a
		// prepared-state commit.
		PlusDrive(const PlusDrive&) = delete;
		PlusDrive& operator=(const PlusDrive&) = delete;
		PlusDrive(PlusDrive&&) noexcept;
		PlusDrive& operator=(PlusDrive&&) noexcept;
		// Flush the committed store on teardown so a normal close never loses the
		// tail of a write burst. Prepared overlays are discarded.
		~PlusDrive();

		void attach(Sim* _sim) { m_sim = _sim; }
		void setMode(const Mode _mode) { m_mode = _mode; }
		Mode getMode() const { return m_mode; }
		void setCannedOrder(const CannedOrder _order) { m_cannedOrder = _order; }

		// A path selects the process-wide committed store for this machine.
		// Prepared machines get a copy-on-write overlay; commitPrepared()
		// promotes it only while its base generation is still current.
		// setServeFlash(true) serves bulk reads from the store.
		bool setBackingPath(const std::string& _path, bool _prepared = false,
			std::string* _error = nullptr);
		// Promote a prepared overlay. A live write after preparation began makes
		// the generation stale, so the caller must discard and retry rather than
		// overwrite newer committed state. A forced persistence failure also
		// returns false and rolls back the in-memory merge.
		bool commitPrepared(PlusDrive& _prepared, std::string* _error = nullptr);
		bool canCommitPrepared(const PlusDrive& _prepared,
			std::string* _error = nullptr) const;
		uint64_t storageGeneration() const;
		void setServeFlash(const bool _serve) { m_serveFlash = _serve; }
		// Model selection also picks the firmware's read-side status-poll
		// windows: MD 1.63 polls its copied internal-SRAM reader at 0x100099c,
		// MM 1.32b at 0x10000c0/0x10000cc and 0x216da0/0x216db0. All are
		// control reads and must not consume a storage nibble. The 0x59 status
		// profile is separate; MM adds a pre-wait/sample step there.
		void setModel(const MachineModel _model) { m_model = _model; }

		// Backing-file writes are coalesced. Both write paths (a bulk 0xf4 phase
		// carrying 512 storage bytes, and a command page) used to rewrite the
		// entire sparse image, so a 1 MiB snapshot slot meant ~2048
		// full-image rewrites: gigabytes of I/O on the emulation thread, seen as
		// a frozen front panel and ~5x slower than real time. Default flush
		// interval 500 ms.
		//
		// A reset, replacement commit or teardown is a persistence barrier, so
		// the boot state word is durable before a reset even when written near
		// the interval boundary. `_force` ignores the interval.
		// GEARMULATOR_PLUSDRIVE_FLUSHMS=N overrides it; 0 restores per-write
		// persistence. Returns false on I/O or validation failure and leaves the
		// store dirty for retry.
		bool flushBacking(bool _force);
		// Number of successful committed backing-file writes (measurement hook).
		uint32_t backingSaves() const;
		std::string persistenceError() const;

		// Called by Sim::computeParallelData() while PPDDR = $04, where the
		// driver clocks data in and expects bits 3-7 driven. Takes the PPDAT
		// latch and CPU PC (PC for diagnostics; receive levels come from the
		// decoded transmit command and are model-agnostic).
		uint8_t dataBitForRead(const uint8_t _ppdatLatch, const uint32_t _pc);
		uint8_t dataBitForReadImpl(const uint8_t _ppdatLatch, const uint32_t _pc);

		// Snooped by Sim::write8 so the device can decode send_raw commands:
		// PPDDR=$0c phase, 6 bytes MSB-first (D5, D6>>24, D6-mid, D6>>8,
		// D6-low, D7). A direction flip prepares the receive response.
		void onPPDDRWrite(const uint8_t _newDdr);
		void onPPDATWrite(const uint8_t _value, const uint8_t _ddr);

		// Reset all per-transaction state (called on Sim reset / direction flips).
		void resetTransaction();

		// Decoded transmit bytes since the last direction flip (for tests/logs).
		uint32_t wordsSeen() const { return m_wordsSeen; }
		uint32_t writePages() const { return m_writePages; }
		uint32_t cmdCount(const uint8_t _cmd) const { return m_cmdStats[_cmd]; }

		// Persisted configuration block. The `0x59 @ 0x1FFFFF` write streams it
		// via the 0xf4 phase (state word 0x4E450007 first); `0x52 @ 0x1FFFFF`
		// reads it back. A dedicated field, so the firmware's out-of-range
		// directory writes (which wrap past the 2 MiB array) cannot clobber it.
		std::vector<uint8_t> configBlock() const;

	private:
		friend class Device;

		// Move a prepared storage transaction between endpoint objects without
		// copying the sparse store. Protocol state stays endpoint-local.
		std::shared_ptr<DriveStore> takeStoreForReboot() noexcept
		{
			return std::move(m_store);
		}
		void adoptStore(std::shared_ptr<DriveStore> _store) noexcept
		{
			m_store = std::move(_store);
			invalidateReadCache();
		}

		Sim* m_sim = nullptr;
		MachineModel m_model = MachineModel::Machinedrum;
		Mode m_mode = Mode::Canned;
		CannedOrder m_cannedOrder = CannedOrder::MsbNormal;
		bool m_lastClock = false;
		uint16_t m_shiftIn = 0;
		uint32_t m_bitCount = 0;
		uint16_t m_lastWord = 0;
		uint32_t m_wordsSeen = 0;
		uint16_t m_txWord = g_statusWord;
		uint32_t m_txBitsLeft = 0;

		// Transmit decoder (DDR=$0c): each PPDAT write carries one data bit
		// MSB-first, and 6 bytes form a send_raw command (D5, D6x3, D6-low, D7).
		bool m_txLastClock = false;
		uint8_t m_txCurByte = 0;
		uint32_t m_txCurBits = 0;
		std::vector<uint8_t> m_txBytes;
		bool m_txOverflow = false;
		uint8_t m_lastDdr = 0;

		// Command state: the last decoded 6-byte command selects the receive
		// levels (0x41 presence magic, 0x40/0x52 init low, else high).
		std::array<uint8_t, 6> m_lastCmd{};
		bool m_lastCmdValid = false;

		// Durable pages and config live in a shared DriveStore; the command and
		// framing fields above stay per-Sim. A prepared machine uses a store
		// overlay, promoted only after a generation check at commit.
		std::shared_ptr<DriveStore> m_store;
		// Sequential reads touch one sector at a time, so cache that page per
		// endpoint rather than taking the DriveStore mutex and doing a map
		// lookup per byte. The generation token covers both committed parent
		// changes and local overlay revisions.
		mutable uint32_t m_cachedSector = 0xffffffffu;
		mutable uint64_t m_cachedGeneration = 0;
		mutable plusdrive::Page m_cachedPage{};
		void invalidateReadCache() noexcept
		{
			m_cachedSector = 0xffffffffu;
			m_cachedGeneration = 0;
		}
		uint8_t readByte(uint64_t _byteAddr) const;
		void writeByte(uint64_t _byteAddr, uint8_t _value);
		bool m_serveFlash = false;
		uint32_t m_bulkBase = 0;
		uint32_t m_bulkPos = 0;
		uint32_t m_cmdReadPos = 0;
		uint32_t m_writePages = 0;
		uint32_t m_bulkWPos = 0;
		uint8_t m_txWByte = 0;
		uint32_t m_bulkWPhase = 0;
		// Writes seen in the CURRENT 0xf4 phase (reset on every phase entry,
		// unlike m_bulkWPhase which counts the whole command). The prologue
		// `f0 f4 00` and the `f4` dismiss marker are per phase, not per command,
		// so the skip and the payload/store budgets must be phase-local while
		// the byte position (m_bulkWPos) keeps accumulating across the phases of
		// one command.
		uint32_t m_wPhaseWrites = 0;
		// WPHASELOG diagnostics: raw opening writes of the current 0xf4 phase
		// and its total write count, so the per-phase prologue can be told
		// apart (the fixed skip of 3 is right for some flows, wrong for the
		// directory read-modify-write loop).
		uint8_t m_phaseVals[8] = {};
		uint32_t m_phaseValsN = 0;
		// ...and the tail of the phase (the dismiss marker the firmware emits
		// after the payload), kept as a ring buffer.
		uint8_t m_phaseTail[8] = {};
		uint32_t m_phaseTailPos = 0;
		uint32_t m_phaseWrites = 0;
		uint32_t m_cmdSeq = 0;
		std::array<uint32_t, 256> m_cmdStats{};
		uint8_t serveFlashNibble(bool _packetFraming = true);
		void persistWritePage();

		// Canned receive bitstream (Mode::Canned): 7 zero preamble bits (D6
		// poll -> enter read path) followed by 0xC0FF8080 MSB-first, shifting
		// out one bit per PPDAT read, repeating per send_raw transaction.
		std::vector<uint8_t> m_cannedBytes;
		uint32_t m_cannedBitPos = 0;
		bool cannedBitAt(const uint32_t _pos, const uint32_t _payload,
			const uint32_t _preamble = 8) const;
	};

	// ---------------------------------------------------------------------------
	// Backing store: a machine-local +Drive flash file, shared across OSes
	// (a separate chip on real hardware; not per-OS like the NOR user space or
	// the -os-upgrade-<fingerprint>.bin images). Excluded from OS hibernation
	// slots by construction: a dedicated file, never a hibernation entry.
	// Format: a 16-byte v2 header, then sparse (u32 sector + 512-byte page)
	// entries and an optional 16-byte config trailer. Saves use a synced
	// sibling plus atomic replacement; an unrecognized file is preserved
	// non-destructively before a fresh image is created.
	// ---------------------------------------------------------------------------
	namespace plusdrive
	{
		static constexpr uint32_t g_formatVersion = 2;
		// Persisted config block trailer appended after the sparse sectors.
		// Kept outside the sectors so the firmware's own writes to the config
		// sector cannot corrupt the boot state word.
		static constexpr uint32_t g_configSize = 16;

		// v2 header: magic "MDPLUSDRV1" (10 bytes) + u16 version + u32 sector
		// count, then that many (u32 sector, 512 bytes) entries, then an
		// optional 16-byte config trailer. v1 (dense 2 MiB) files are no longer
		// loadable and are preserved as .bak by ensureReady.
		std::string filename(const std::string& _homePath, MachineModel _model);
		bool load(Pages& _pages, const std::string& _path,
			std::vector<uint8_t>* _config = nullptr, std::string* _error = nullptr);
		bool save(const std::string& _path, const Pages& _pages,
			const std::vector<uint8_t>& _config = {}, std::string* _error = nullptr);
		bool ensureReady(Pages& _pages, const std::string& _path,
			std::string* _error = nullptr, std::vector<uint8_t>* _config = nullptr);
	}
}
