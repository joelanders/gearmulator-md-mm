#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "mc68k/mc68k.h"
#include "mc68k/hdi08.h"

#include "mdflash.h"
#include "mdsim.h"
#include "mdturbomidi.h"
#include "mdtypes.h"

#include "synthLib/midiBufferParser.h"

namespace md
{
	class Rom;
	class FrontPanel;

	// ColdFire MCF5206E microcontroller for the Elektron Machinedrum.
	//
	// This is the memory-map backend of the machine: it selects the ColdFire CPU
	// type on the shared Musashi core and routes the 32-bit ColdFire address space
	// to the ROM/RAM regions. The SIM peripheral window (0x300000) is modelled by
	// md::Sim; the two DSP HI08 host-port windows (0x500000 / 0x600000) are backed by
	// real dsp56kEmu DSP56303s (md::Dsp).
	//
	// Address map (from the MAME driver's elektron_map; see COLDFIRE notes):
	//   0x00000000-0x000fffff  ROM  (flash low 1 MB)
	//   0x00100000-0x001fffff  patch RAM (bootstrap)            aliased at 0x00700000 (OS)
	//   0x00200000-0x002fffff  main RAM                         aliased at 0x20000000 / 0x40000000
	//   0x00300000-0x0030ffff  SIM peripheral window            (trap-logged)
	//   0x00310000-0x003fffff  upper loader RAM
	//   0x00500000-0x00500007  DSP 1 HI08                       (md::Dsp)
	//   0x00600000-0x00600007  DSP 2 HI08                       (md::Dsp)
	//   0x01000000-0x01001fff  ColdFire internal SRAM (8 KB)
	//   0x10000000-0x107fffff  ROM  (full 8 MB flash)
	class Microcontroller final : public mc68k::Mc68k, public MidiByteSink
	{
	public:
		Microcontroller(const Rom& _rom, MachineModel _model = MachineModel::Machinedrum,
			const std::vector<uint8_t>& _initialPatchRam = {},
			const std::vector<uint8_t>& _initialMainRam = {});

		// mc68k::Mc68k overrides
		uint32_t exec() override;
		uint32_t readIrqUserVector(uint8_t _level) override;
		uint16_t readImm16(uint32_t _addr) override;

		// Let the CPU core raise its standard illegal-instruction exception.
		uint32_t onIllegalInstruction(uint32_t _opcode) override;

		uint8_t  read8 (uint32_t _addr) override;
		uint16_t read16(uint32_t _addr) override;
		void     write8 (uint32_t _addr, uint8_t  _val) override;
		void     write16(uint32_t _addr, uint16_t _val) override;

		uint32_t getResetPC() override;
		uint32_t getResetSP() override;
		void prepareFirmwareUpdateBoot(uint32_t _factoryFlashAddress);

		// ColdFire-facing HI08 register files for the two DSPs. The Hardware owns the
		// md::Dsp wrappers and registers their boot/bridge callbacks on these; the
		// Microcontroller just maps them into the address space (0x500000 / 0x600000).
		mc68k::Hdi08& getHdi08Dsp1() { return m_hdi08Dsp1; }	// DSP1 = mixer/codec
		mc68k::Hdi08& getHdi08Dsp2() { return m_hdi08Dsp2; }	// DSP2 = voice producer

		Sim& getSim() { return m_sim; }

		// Attach a front-panel decoder to receive the post-handshake UART2 host->panel
		// stream (LCD framebuffer + LED banks). Owned by the caller (md::Hardware).
		void setFrontPanel(FrontPanel* _fp) { m_frontPanel = _fp; }

		// Present a panel->host byte to the firmware over UART2 RX (the same channel as the
		// startup handshake). Used to deliver button/encoder events. Call from the CPU thread.
		bool tryQueuePanelRx(uint8_t _byte)
		{
			return m_sim.tryQueueRx(Sim::g_uartPanel, _byte);
		}
		void queuePanelRx(uint8_t _byte) { (void)tryQueuePanelRx(_byte); }
		size_t availablePanelRxBytes() const
		{
			return m_sim.availableRxBytes(Sim::g_uartPanel);
		}

		// Present a MIDI byte to the firmware over UART1 RX. Call from the CPU thread.
		bool tryQueueMidiRx(uint8_t _byte)
		{
			return m_sim.tryQueueRx(Sim::g_uartMidi, _byte);
		}
		bool tryWriteMidiByte(uint8_t _byte) override
		{
			return tryQueueMidiRx(_byte);
		}
		void queueMidiRx(uint8_t _byte) { (void)tryQueueMidiRx(_byte); }
		size_t queuedMidiRxBytes() const
		{
			return m_sim.queuedRxBytes(Sim::g_uartMidi);
		}
		size_t queuedMidiByteCount() const override
		{
			return queuedMidiRxBytes();
		}
		size_t availableMidiRxBytes() const
		{
			return m_sim.availableRxBytes(Sim::g_uartMidi);
		}
		size_t midiRxOverflowCount() const
		{
			return m_sim.rxOverflowCount(Sim::g_uartMidi);
		}
		uint64_t midiRxConsumedCount() const
		{
			return m_sim.rxConsumedCount(Sim::g_uartMidi);
		}

		struct PatchByteUpdate
		{
			uint32_t address = 0;
			uint8_t value = 0;
			uint8_t mask = 0xff;
		};

		// Audio-owner helpers for small semantic updates. They never wait behind a
		// state snapshot: contention returns false and the caller retries at a later
		// block boundary. All addresses are validated before an update is committed.
		bool tryUpdatePatchBytes(const PatchByteUpdate* _updates, size_t _count);
		bool tryReadPatchBytes(const uint32_t* _addresses, uint8_t* _values,
			size_t _count);

		// Drain complete MIDI messages written by the firmware to UART1 TX.
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut);
		uint64_t midiTxOverflowCount() const
		{
			return m_midiTxOverflow.load(std::memory_order_relaxed);
		}
		// Observe raw UART1 TX bytes without consuming normal MIDI output.
		void setMidiTransmitTap(std::function<void(uint8_t)> _tap)
		{
			m_midiTransmitTap = std::move(_tap);
		}

		std::vector<uint8_t> copyPatchRam() const;


	private:
		// A resolved backing store for an address. If 'peripheral' is set the address
		// falls in an unmodelled peripheral / unmapped window (trap-logged by the caller).
		struct Region
		{
			uint8_t* data      = nullptr;
			uint32_t offset    = 0;
			uint32_t size      = 0;
			bool     writable  = false;
			bool     peripheral = false;
		};

		Region resolve(uint32_t _addr);
		void logPeripheral(uint32_t _addr, uint32_t _value, uint8_t _size, bool _write);
		void onPanelTransmit(uint8_t _byte);	// startup reply modeled from the public MAME driver

		// Match MAME's panel-ready notification after the startup handshake.
		// Runs on the CPU thread.
		void panelDisplayReadyPost();

		uint32_t readMem32(uint32_t _addr);
		void     writeMem32(uint32_t _addr, uint32_t _value);

		const MachineModel m_model;
		const Rom& m_rom;
		FlashCommandDecoder m_flashCommands;
		std::vector<uint8_t> m_flashData;

		Sim m_sim;	// on-chip SIM peripheral window (MBAR base 0x300000)
		struct MidiTxBuffer
		{
			std::array<uint8_t, Sim::g_uartTxCapacity> bytes{};
			size_t size = 0;
		};

		// UART production swaps between two fixed buffers. The producer mutex protects
		// both the active index and its write cursor; the drain mutex serializes the
		// stateful MIDI parser while a detached buffer is consumed.
		mutable std::mutex m_midiTxMutex;
		std::mutex m_midiTxDrainMutex;
		std::array<MidiTxBuffer, 2> m_midiTxBuffers;
		size_t m_midiTxProducerIndex = 0;
		size_t m_midiTxDrainIndex = 1;
		bool m_midiTxDiscontinuity = false;
		std::atomic<uint64_t> m_midiTxOverflow{0};
		std::function<void(uint8_t)> m_midiTransmitTap;
		synthLib::MidiBufferParser m_midiTxParser{synthLib::MidiEventSource::Device};

		// ColdFire-facing HI08 register files for the two DSP host-port windows. The
		// md::Dsp wrappers (owned by md::Hardware) attach boot/bridge callbacks to these;
		// mdmc routes 0x500000-7 / 0x600000-7 to them (see read/write below).
		mc68k::Hdi08 m_hdi08Dsp1;	// DSP 1 HI08 window at 0x00500000..0x00500007
		mc68k::Hdi08 m_hdi08Dsp2;	// DSP 2 HI08 window at 0x00600000..0x00600007

		// RAM regions. Bytes are stored in natural (big-endian / 68k) order, matching
		// the firmware image, so mc68k::memoryOps read/write helpers work directly.
		std::vector<uint8_t> m_patchRam;		// 0x00100000..0x001fffff (aliased at 0x00700000)
		mutable std::shared_mutex m_patchRamMutex;
		std::vector<uint8_t> m_mainRam;			// 0x00200000..0x002fffff (aliased at 0x20000000 / 0x40000000)
		std::vector<uint8_t> m_loaderRam;		// 0x00310000..0x003fffff
		std::vector<uint8_t> m_internalSram;	// 0x01000000..0x0100ffff (64 KB)

		// Instruction and extension fetches are normally sequential within one
		// host-backed page. Cache only that page's pointer, never its contents:
		// writes remain immediately visible to self-modifying RAM code.
		uint32_t m_immPageAddress = 0xffffffffu;
		const uint8_t* m_immPageData = nullptr;

		uint8_t  m_panelProbeIndex = 0;	// progress matching the UART2 startup probe
		uint8_t  m_mmPanelProbeIndex = 0;
		bool     m_mmPanelHandshakeDone = false;

		bool     m_panelDisplayReady = false;	// enabled once the panel startup handshake completes
		uint32_t m_panelDisplayReadyDivider = 0;	// rate-limits the periodic semaphore post

		void advanceAfterCpu(uint32_t _cycles);
		void serviceExternalIrq4();
		bool m_externalIrq4Pending = false;
		uint8_t m_externalIrq4PendingLevel = 0;
		uint8_t m_externalIrq4PendingVector = 0;


		FrontPanel* m_frontPanel = nullptr;	// optional LCD/LED decoder (owned by md::Hardware)
	};
}
