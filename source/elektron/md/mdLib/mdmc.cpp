#include "mdmc.h"

#include "mdfrontpanel.h"
#include "mdmemorymap.h"
#include "mdrom.h"

#include "mc68k/memoryOps.h"
#include "mc68k/Musashi/m68k.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>

// Provide the Musashi memory-access callbacks (m68k_read_memory_*, _pcrelative_*, etc.)
// for this microcontroller. Exactly one TU per synth includes this - cf. n2xmc.cpp / xtUc.cpp.
#define MC68K_CLASS md::Microcontroller
#include "mc68k/musashiEntry.h"

namespace md
{
	namespace
	{
		// The SFX-60 MKII stores user DigiPRO waves in the uniform-sector portion
		// of its AMD-compatible flash. Its sectors differ from the MD bottom-boot
		// part handled by FlashCommandDecoder.
		class MonomachineFlash final : public hwLib::Am29f
		{
		public:
			MonomachineFlash(uint8_t* const _data, const size_t _size)
				: Am29f(_data, _size, false, true), m_size(_size)
			{
			}

			bool eraseSector(const uint32_t _address) const override
			{
				// Bottom-boot geometry: eight 8 KiB boot blocks below 64 KiB,
				// uniform 64 KiB sectors above. The factory upgrader erases
				// 0x4000-0xFFFF (sparing the reset-vector sectors) then the
				// 64 KiB blocks; anything else is not a real sector.
				constexpr size_t bootLimit = 64 * 1024;
				constexpr size_t bootSize = 8 * 1024;
				constexpr size_t sectorSize = 64 * 1024;
				if(_address < bootLimit)
				{
					if((_address % bootSize) != 0 || bootSize > m_size - _address)
						return false;
					return Am29f::eraseSector(_address, bootSize / 1024);
				}
				if((_address % sectorSize) != 0 || _address > m_size
					|| sectorSize > m_size - _address)
					return false;
				return Am29f::eraseSector(_address, sectorSize / 1024);
			}

		private:
			const size_t m_size;
		};

		constexpr auto makeMmPanelStartupProbe()
		{
			std::array<uint8_t, 30> probe{};
			std::size_t index = 0;
			for(uint8_t bank = 0x20; bank <= 0x2d; ++bank)
			{
				probe[index++] = bank;
				probe[index++] = 0xff;
			}
			probe[index++] = 0x30;
			probe[index] = 0x00;
			return probe;
		}

		constexpr auto g_mmPanelStartupProbe = makeMmPanelStartupProbe();
		static_assert(g_mmPanelStartupProbe.size() == 30);
		static_assert(g_mmPanelStartupProbe.front() == 0x20);
		static_assert(g_mmPanelStartupProbe[27] == 0xff);
		static_assert(g_mmPanelStartupProbe[28] == 0x30);
		static_assert(g_mmPanelStartupProbe.back() == 0x00);
	}

	Microcontroller::Microcontroller(const Rom& _rom, const MachineModel _model,
		const std::vector<uint8_t>& _initialPatchRam,
		const std::vector<uint8_t>& _initialFlash)
		: Microcontroller(_rom, _model, _initialPatchRam, _initialFlash, {})
	{
	}

	Microcontroller::Microcontroller(const Rom& _rom, const MachineModel _model,
		const std::vector<uint8_t>& _initialPatchRam,
		const std::vector<uint8_t>& _initialFlash,
		const std::vector<uint8_t>& _initialUserFlash)
		: Mc68k(M68K_CPU_TYPE_MCF5206E)
		, m_model(_model)
		, m_rom(_rom)
		, m_flashData(_rom.data())
		, m_patchRam(memorymap::g_patchBootstrap.size(), 0)
		, m_mainRam(memorymap::g_mainRam.size(), 0)
		, m_loaderRam(memorymap::g_loaderRam.size(), 0)
		, m_internalSram(memorymap::g_internalSram.size(), 0)
	{
		if((m_model == MachineModel::Machinedrum
			|| m_model == MachineModel::Monomachine)
			&& _initialFlash.size() == m_flashData.size())
			m_flashData = _initialFlash;
		if(m_model == MachineModel::Monomachine
			&& _initialUserFlash.size() == memorymap::g_mmUserFlash.size()
			&& m_flashData.size() >= memorymap::g_flashFull.offset(
				memorymap::g_mmUserFlash.end))
		{
			const auto offset = memorymap::g_flashFull.offset(
				memorymap::g_mmUserFlash.begin);
			std::copy(_initialUserFlash.begin(), _initialUserFlash.end(),
				m_flashData.begin() + offset);
		}
		if(m_model == MachineModel::Monomachine && !m_flashData.empty())
			m_monomachineFlash = std::make_unique<MonomachineFlash>(
				m_flashData.data(), m_flashData.size());

		// Report the MKII board profile used by both supported targets.
		m_sim.setMk2PortAInvertedLoopback(true);

		// The panel controller is not part of the emulator. Supply the minimal
		// firmware-observed UART startup exchange here.
		m_sim.setTransmitCallback(Sim::g_uartPanel, [this](const uint8_t _b) { onPanelTransmit(_b); });
		m_sim.setTransmitCallback(Sim::g_uartMidi, [this](const uint8_t _b)
		{
			{
				const std::scoped_lock lock(m_midiTxMutex);
				auto& producer = m_midiTxBuffers[m_midiTxProducerIndex];
				if(producer.size < producer.bytes.size())
				{
					producer.cycles[producer.size] = getCycles();
					producer.bytes[producer.size++] = _b;
				}
				else
				{
					m_midiTxDiscontinuity = true;
					m_midiTxOverflow.fetch_add(1, std::memory_order_relaxed);
				}
			}
			if(m_midiTransmitTap)
				m_midiTransmitTap(_b);
		});


		// A complete patch-RAM image is already initialized and can be restored as-is.
		if(_initialPatchRam.size() == m_patchRam.size())
			m_patchRam = _initialPatchRam;
		// The patch window reads flash until the decompressed OS lands in RAM
		// (or a restored image already carrying the DAVE marker runs from RAM).
		resetPatchWindowMark();
	}

	std::vector<uint8_t> Microcontroller::copyPatchRam() const
	{
		std::shared_lock lock(m_patchRamMutex);
		return m_patchRam;
	}

	bool Microcontroller::replacePatchRam(const std::vector<uint8_t>& _data)
	{
		if(_data.size() != m_patchRam.size())
			return false;
		std::unique_lock lock(m_patchRamMutex);
		m_patchRam = _data;
		return true;
	}
	std::vector<uint8_t> Microcontroller::copyFlashData() const
	{
		std::shared_lock lock(m_flashMutex);
		return m_flashData;
	}

	std::vector<uint8_t> Microcontroller::copyUserFlash() const
	{
		if(m_model != MachineModel::Monomachine
			|| m_flashData.size() < memorymap::g_flashFull.offset(
				memorymap::g_mmUserFlash.end))
			return {};
		std::shared_lock lock(m_flashMutex);
		const auto begin = m_flashData.begin() + memorymap::g_flashFull.offset(
			memorymap::g_mmUserFlash.begin);
		return {begin, begin + memorymap::g_mmUserFlash.size()};
	}

	bool Microcontroller::copyFlashDataRangeRealtime(uint8_t* const _destination,
		const size_t _offset, const size_t _size) const
	{
		if(!_destination || _offset > m_flashData.size()
			|| _size > m_flashData.size() - _offset)
			return false;
		std::copy_n(m_flashData.begin() + _offset, _size, _destination);
		return true;
	}

	uint64_t Microcontroller::flashIdleCycles() const
	{
		return m_flashDirty && getCycles() >= m_lastFlashWriteCycle
			? getCycles() - m_lastFlashWriteCycle : 0;
	}

	bool Microcontroller::replaceFlashData(const std::vector<uint8_t>& _data,
		const bool _dirty)
	{
		if(m_model != MachineModel::Machinedrum || _data.size() != m_flashData.size())
			return false;
		std::unique_lock flashLock(m_flashMutex);
		m_flashData = _data;
		m_flashCommands = {};
		m_immPageAddress = 0xffffffffu;
		m_immPageData = nullptr;
		m_flashDirty = _dirty;
		m_lastFlashWriteCycle = getCycles();
		resetPatchWindowMark();
		return true;
	}

	bool Microcontroller::exchangeFlashState(Microcontroller& _other)
	{
		if(this == &_other)
			return true;
		if(m_model != MachineModel::Machinedrum || m_model != _other.m_model
			|| m_flashData.size() != _other.m_flashData.size())
			return false;
		std::scoped_lock lock(m_flashMutex, _other.m_flashMutex);
		m_flashData.swap(_other.m_flashData);
		std::swap(m_flashDirty, _other.m_flashDirty);
		m_flashCommands = {};
		_other.m_flashCommands = {};
		m_immPageAddress = 0xffffffffu;
		_other.m_immPageAddress = 0xffffffffu;
		m_immPageData = nullptr;
		_other.m_immPageData = nullptr;
		// A write-cycle count belongs to its emulator timeline, not to the moved
		// backing store. Conservatively restart each dirty-idle interval.
		m_lastFlashWriteCycle = getCycles();
		_other.m_lastFlashWriteCycle = _other.getCycles();
		// Each side keeps its own patch RAM: re-derive the window read source.
		resetPatchWindowMark();
		_other.resetPatchWindowMark();
		return true;
	}

	Microcontroller::StateImagePublishResult Microcontroller::publishStateImagesRealtime(
		std::vector<uint8_t>& _flash, std::vector<uint8_t>& _patchRam,
		const bool _dirty)
	{
		if(m_model != MachineModel::Machinedrum
			|| _flash.size() != m_flashData.size()
			|| _patchRam.size() != m_patchRam.size())
			return StateImagePublishResult::Invalid;
		std::unique_lock flashLock(m_flashMutex, std::try_to_lock);
		if(!flashLock.owns_lock())
			return StateImagePublishResult::Busy;
		std::unique_lock patchLock(m_patchRamMutex, std::try_to_lock);
		if(!patchLock.owns_lock())
			return StateImagePublishResult::Busy;
		// This is called only by the scheduler owner between executed instructions.
		// With both host snapshot locks held, every observer sees complete backing
		// stores rather than an incrementally modified or concurrently exchanged one.
		m_flashData.swap(_flash);
		m_patchRam.swap(_patchRam);
		m_flashCommands = {};
		m_immPageAddress = 0xffffffffu;
		m_immPageData = nullptr;
		m_flashDirty = _dirty;
		m_lastFlashWriteCycle = getCycles();
		resetPatchWindowMark();
		return StateImagePublishResult::Published;
	}
	void Microcontroller::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut, const uint64_t _nativeOrigin)
	{
		// MidiBufferParser is stateful (including partial messages), so only one
		// consumer may detach and parse a UART batch at a time.
		const std::scoped_lock drainLock(m_midiTxDrainMutex);
		size_t drainIndex = 0;
		bool discontinuity = false;
		{
			const std::scoped_lock lock(m_midiTxMutex);
			if(m_midiTxBuffers[m_midiTxProducerIndex].size == 0)
				return;
			std::swap(m_midiTxProducerIndex, m_midiTxDrainIndex);
			drainIndex = m_midiTxDrainIndex;
			discontinuity = m_midiTxDiscontinuity;
			m_midiTxDiscontinuity = false;
		}

		auto& drain = m_midiTxBuffers[drainIndex];
		for(size_t i = 0; i < drain.size; ++i)
		{
			const auto cycles = drain.cycles[i];
			const auto sample = (cycles / g_ucClockHz) * g_samplerate
				+ ((cycles % g_ucClockHz) * g_samplerate + g_ucClockHz - 1) / g_ucClockHz;
			const auto offset = sample > _nativeOrigin ? sample - _nativeOrigin : 0;
			m_midiTxParser.write(drain.bytes[i], static_cast<uint32_t>(
				std::min<uint64_t>(offset, std::numeric_limits<uint32_t>::max())));
		}
		m_midiTxParser.getEvents(_midiOut);
		if(discontinuity)
			m_midiTxParser.discardPartialMessage();
		drain.size = 0;
	}

	Microcontroller::Region Microcontroller::patchWindowRegion(const uint32_t _addr)
	{
		// Both patch aliases share one RAM backing at identical offsets; the
		// flash backing is the same cells in the 8 MiB image.
		const uint32_t ramOffset = _addr & 0x000fffffu;
		const bool flash =
			(m_patchFlashSectors.load(std::memory_order_relaxed) & (1u << patchWindowSector(_addr))) != 0;
		if(!flash && ramOffset < m_patchRam.size())
		{
			Region r;
			r.data = m_patchRam.data();
			r.offset = ramOffset;
			r.size = static_cast<uint32_t>(m_patchRam.size());
			r.writable = true;
			return r;
		}
		const uint32_t flashOffset = patchWindowFlashOffset(_addr);
		Region r;
		r.data = m_flashData.data();
		r.offset = flashOffset;
		r.size = static_cast<uint32_t>(m_flashData.size());
		r.writable = false;
		return r;
	}

	void Microcontroller::setPatchWindowSectorFlash(const uint32_t _addr, const bool _flash)
	{
		// The OS upgrader verifies whole-image checksums after programming.
		// Sectors it never rewrote (identical content, skipped) must still
		// read the flash image rather than pre-upgrade RAM, so the first
		// executed flash command in this window selects flash for every
		// sector. Later plain writes still select RAM per sector as before.
		if(_flash && !m_patchFlashUpgradeArmed.exchange(true, std::memory_order_acq_rel))
		{
			m_patchFlashSectors.store(0xffff, std::memory_order_relaxed);
			m_immPageAddress = 0xffffffffu;
			m_immPageData = nullptr;
		}
		const auto bit = static_cast<uint16_t>(1u << patchWindowSector(_addr));
		auto marks = m_patchFlashSectors.load(std::memory_order_relaxed);
		const auto updated = _flash ? static_cast<uint16_t>(marks | bit)
			: static_cast<uint16_t>(marks & ~bit);
		if(updated != marks)
		{
			m_patchFlashSectors.store(updated, std::memory_order_relaxed);
			// Read source changed underfoot: drop the cached fetch page so the
			// next instruction fetch resolves against the new backing.
			m_immPageAddress = 0xffffffffu;
			m_immPageData = nullptr;
		}
	}

	void Microcontroller::resetPatchWindowMark()
	{
		// Sectors start as RAM: early reads (before any writer runs) see
		// zeros like they always did. Only an executed flash command flips
		// its sector to flash; plain writes keep RAM. A restored image runs
		// from RAM either way, so no marker check is needed.
		m_patchFlashSectors.store(0x0000, std::memory_order_relaxed);
		m_patchFlashUpgradeArmed.store(false, std::memory_order_relaxed);
		m_immPageAddress = 0xffffffffu;
		m_immPageData = nullptr;
	}

	Microcontroller::Region Microcontroller::resolve(const uint32_t _addr)
	{
		auto ram = [](std::vector<uint8_t>& _buf, const uint32_t _offset) -> Region
		{
			Region r;
			r.data = _buf.data();
			r.offset = _offset;
			r.size = static_cast<uint32_t>(_buf.size());
			r.writable = true;
			return r;
		};

		auto rom = [this](const uint32_t _offset) -> Region
		{
			Region r;
			r.data = m_flashData.data();	// writes go through the flash command state machine
			r.offset = _offset;
			r.size = static_cast<uint32_t>(m_flashData.size());
			r.writable = false;
			return r;
		};

		auto peripheral = []() -> Region
		{
			Region r;
			r.peripheral = true;
			return r;
		};

		if(memorymap::g_flashLow.contains(_addr))			return rom(memorymap::g_flashLow.offset(_addr));
		if(memorymap::g_patchBootstrap.contains(_addr))	return ram(m_patchRam, memorymap::g_patchBootstrap.offset(_addr));
		if(memorymap::g_mainRam.contains(_addr))			return ram(m_mainRam, memorymap::g_mainRam.offset(_addr));
		if(memorymap::g_sim.contains(_addr))				return peripheral();
		if(memorymap::g_loaderRam.contains(_addr))		return ram(m_loaderRam, memorymap::g_loaderRam.offset(_addr));
		if(memorymap::g_dsp1Hdi08.contains(_addr))		return peripheral();
		if(memorymap::g_dsp2Hdi08.contains(_addr))		return peripheral();
		if(memorymap::g_patchOsAlias.contains(_addr))		return ram(m_patchRam, memorymap::g_patchOsAlias.offset(_addr));
		if(memorymap::g_internalSram.contains(_addr))		return ram(m_internalSram, memorymap::g_internalSram.offset(_addr));
		if(memorymap::g_flashFull.contains(_addr))		return rom(memorymap::g_flashFull.offset(_addr));
		if(memorymap::g_mainHighAlias.contains(_addr))	return ram(m_mainRam, memorymap::g_mainHighAlias.offset(_addr));
		if(memorymap::g_mainExecAlias.contains(_addr))	return ram(m_mainRam, memorymap::g_mainExecAlias.offset(_addr));

		return peripheral();									// unmapped
	}

	void Microcontroller::logPeripheral(const uint32_t _addr, const uint32_t _value, const uint8_t _size, const bool _write)
	{
		(void)_addr;
		(void)_value;
		(void)_size;
		(void)_write;
	}

	void Microcontroller::onPanelTransmit(const uint8_t _byte)
	{
		// Minimal Monomachine panel handshake. Firmware disassembly establishes
		// the 0xcc autobaud response; 0x23,0x01 is the compatible descriptor used
		// by the earlier private bring-up implementation.
		if(m_model == MachineModel::Monomachine)
		{
		// The exchange consists of an autobaud reply, a startup probe, and a
		// compact panel descriptor.
		// BOOT MODE: the descriptor's second byte lands in the firmware boot
		// flag, which the boot check compares against exactly 2. A natural
		// panel answers 0x01; with FUNCTION held it answers 0x02.
		constexpr uint8_t cfg = 0x01;
		const uint8_t descArg = m_bootHoldFunction ? 0x02 : cfg;
		constexpr uint8_t s4  = 0x40;


			if(_byte == 0xaa && !m_mmPanelHandshakeDone)
			{
				m_sim.queueRx(Sim::g_uartPanel, 0xcc);
				return;
			}

			// MM extended startup probe: banks 0x20..0x2d each followed by 0xff, then 0x30,0x00.
			if(!m_mmPanelHandshakeDone && _byte == g_mmPanelStartupProbe[m_mmPanelProbeIndex])
			{
				if(++m_mmPanelProbeIndex == g_mmPanelStartupProbe.size())
				{
					m_mmPanelProbeIndex = 0;
				// Descriptor reply. Queue the optional follow-up value as well;
				// both stages share the UART receive FIFO.
				m_sim.queueRx(Sim::g_uartPanel, 0x23);
				m_sim.queueRx(Sim::g_uartPanel, descArg);
					if(cfg == 0x02)
					{
						m_sim.queueRx(Sim::g_uartPanel, 0x20);	// deep-path stage-4 terminator cmd
						m_sim.queueRx(Sim::g_uartPanel, s4);	// stage-4 status arg (nonzero)
					}
					else
					{
						m_mmPanelHandshakeDone = true;	// shortcut path: no further panel reads before boot
					}
					m_panelDisplayReady = true;
				}
				return;
			}
			if(!m_mmPanelHandshakeDone)
				m_mmPanelProbeIndex = (_byte == g_mmPanelStartupProbe.front()) ? 1 : 0;

			// Post-handshake UART2 traffic is host->panel (LCD tiles / LED banks).
			if(m_panelDisplayReady && m_frontPanel)
				decodePanelByte(_byte);
			return;
		}
		// ===== end MM panel handshake ====================================================

		// Once the startup handshake is done, the UART2 stream is the host->panel traffic:
		// KS0108 LCD framebuffer writes + LED bank updates. Forward it to the front-panel
		// decoder (if one is attached) so the reconstructed display can be observed.
		if(m_panelDisplayReady && m_frontPanel)
			decodePanelByte(_byte);

		// Match the Machinedrum startup probe observed in the firmware UART stream.
		static constexpr uint8_t probe[] =
			{ 0x20, 0xff, 0x21, 0xff, 0x22, 0xff, 0x23, 0xff, 0x24, 0xff, 0x25, 0xff, 0x30, 0x00 };

		if(_byte == probe[m_panelProbeIndex])
		{
			if(++m_panelProbeIndex == sizeof(probe))
			{
				m_panelProbeIndex = 0;
				// The 0x24,0x00,0x00 reply was found by tracing the firmware's
				// receive path and advances it from panel probing into DSP setup.
				// Startup reply, matching MAME panel_send_startup_reply's default: ready
				// signature 0x24, then the panel "startup flags" byte (MAME's PANEL config
				// ioport, whose default is 0x00 - a DIP-style panel-variant selector), then
				// the model/status byte 0x00. 0x00 is the correct default, not a placeholder.
				// BOOT MODE experiment: with FUNCTION held (row 0x24, mask 0x02),
				// report the FUNCTION bit in the flags byte.
				m_sim.queueRx(Sim::g_uartPanel, 0x24);	// startup ready signature
				m_sim.queueRx(Sim::g_uartPanel,
					m_bootHoldFunction ? 0x02 : 0x00);	// startup flags (PANEL config default)
				m_sim.queueRx(Sim::g_uartPanel, 0x00);	// model / status

				// The panel is now present, so subsequent bytes are LCD/LED traffic.
				m_panelDisplayReady = true;
			}
		}
		else
		{
			m_panelProbeIndex = (_byte == probe[0]) ? 1 : 0;
		}
	}

	void Microcontroller::decodePanelByte(const uint8_t _byte)
	{
		if(!m_frontPanel)
			return;
		const auto transition = m_frontPanel->processByte(_byte);
		if(transition && m_panelLedTransitionCallback)
			m_panelLedTransitionCallback(transition->command, transition->value,
				getCycles());
	}

	uint32_t Microcontroller::exec()
	{

		// Step the CPU one instruction, then advance the derived SIM and interrupt wiring.
		// Temporary BOOT MODE probe (GEARMULATOR_MDMM_BOOT_PC): log entry into
		// MD TEST MODE regions once each.
		if(m_model == MachineModel::Machinedrum)
		{
			static const bool trace = std::getenv("GEARMULATOR_MDMM_BOOT_PC") != nullptr;
			if(trace)
			{
				static bool seenTest = false, seenC1e = false;
				const auto pc = getCpuState()->pc;
				if(!seenTest && pc >= 0x1f30 && pc < 0x1f60)
				{
					seenTest = true;
					std::fprintf(stderr, "[PC] entered MD TEST path @pc=%08x cyc=%llu\n",
						pc, static_cast<unsigned long long>(getCycles()));
				}
				if(!seenC1e && pc >= 0xc00 && pc < 0xd00)
				{
					seenC1e = true;
					std::fprintf(stderr, "[PC] entered MD $c1e @pc=%08x cyc=%llu\n",
						pc, static_cast<unsigned long long>(getCycles()));
				}
			}
		}
		const auto cycles = execInstruction();
		advanceAfterCpu(cycles);
		return cycles;
	}

	uint32_t Microcontroller::idleSelfBranchInstructions(uint32_t _maxCycles)
	{
#if M68K_INSTRUCTION_HOOK != OPT_OFF || M68K_EMULATE_TRACE != OPT_OFF \
	|| M68K_MONITOR_PC != OPT_OFF || M68K_EMULATE_FC != OPT_OFF \
	|| M68K_EMULATE_PREFETCH != OPT_OFF
		return 0;
#else
		const auto& cpu = *getCpuState();
		// Only the architectural BRA.B -2 fixed point, after one real execution.
		// No firmware location, memory signature or assumed idle task is used.
		if(cpu.cpu_type != CPU_TYPE_COLDFIRE || cpu.ir != 0x60fe
			|| cpu.pc != cpu.ppc || cpu.stopped || cpu.reset_cycles
			|| cpu.pmmu_enabled || cpu.run_mode != RUN_MODE_NORMAL
			|| cpu.nmi_pending || cpu.int_level > cpu.int_mask
			|| cpu.t1_flag || cpu.t0_flag || cpu.cyc_instruction[0x60fe] != 2
			|| cpu.m68ki_initial_cycles != 1 || cpu.m68ki_remaining_cycles != -1
			|| m_sim.needsInterruptCheck() || m_sim.externalIrq4Asserted()
			|| m_externalIrq4Pending || readImm16(cpu.pc) != 0x60fe)
			return 0;

		// Stop strictly before a timer interrupt or panel-UART character completion.
		// The normal single-instruction path crosses that event and materializes it.
		for(const auto deadline : {m_sim.cyclesUntilNextTimerInterrupt(),
			m_sim.cyclesUntilNextUartTransmit()})
		{
			if(deadline == Sim::g_noTimerInterruptDeadline)
				continue;
			if(!deadline)
				return 0;
			_maxCycles = std::min(_maxCycles, deadline - 1);
		}
		const uint32_t instructions = _maxCycles / 2;
		return instructions >= 8 ? instructions : 0;
#endif
	}

	void Microcontroller::advanceIdleSelfBranch(const uint32_t _instructions)
	{
		// The qualified branch changes no registers or memory. Its previous
		// PC, instruction register and one-instruction cycle accounting remain
		// exactly the values left by the preceding real execution.
		const uint32_t cycles = _instructions * 2;
		m_cycles += cycles;
		advanceAfterCpu(cycles);
	}

	uint32_t Microcontroller::readIrqUserVector(const uint8_t _level)
	{
		const auto vector = Mc68k::readIrqUserVector(_level);
		if(m_externalIrq4Pending && _level == m_externalIrq4PendingLevel
			&& vector == m_externalIrq4PendingVector)
		{
			m_externalIrq4Pending = false;
		}
		return vector;
	}

	void Microcontroller::serviceExternalIrq4()
	{
		// External IRQ4 (DSP2 HI08 HREQ, level-sensitive; the line is set in
		// md::Hardware::pumpDsp2HostRequest). Keep exactly one IRQ4 pending while the
		// line stays asserted. Remember that queued vector directly instead of scanning
		// the CPU's pending deque after every emulated instruction. Interrupt acknowledge
		// clears the flag above, so a still-asserted line is offered again on the next
		// instruction boundary.
		uint8_t level, vector;
		if(m_sim.getExternalIrq4(level, vector))
		{
			if(m_externalIrq4Pending && (level != m_externalIrq4PendingLevel
				|| vector != m_externalIrq4PendingVector))
			{
				// ICR4 was reprogrammed while HREQ remained asserted. Replace the old
				// request rather than delivering a stale level/vector before the new one.
				removePendingInterrupt(m_externalIrq4PendingVector,
					m_externalIrq4PendingLevel);
				m_externalIrq4Pending = false;
			}

			if(!m_externalIrq4Pending)
			{
				injectInterrupt(vector, level);
				m_externalIrq4Pending = true;
				m_externalIrq4PendingLevel = level;
				m_externalIrq4PendingVector = vector;
			}
		}
		else if(m_externalIrq4Pending)
		{
			// A masked or deasserted level line no longer requests service. Remove the
			// exact vector that was queued, even if firmware changed ICR4 in between.
			removePendingInterrupt(m_externalIrq4PendingVector,
				m_externalIrq4PendingLevel);
			m_externalIrq4Pending = false;
		}
	}

	void Microcontroller::advanceAfterCpu(const uint32_t _cycles)
	{
		m_sim.exec(_cycles);

		// Deliver any pending SIM interrupts to the CPU. takeNextInterrupt consumes each edge
		// internally (the RTOS timer tick, the UART transmitter-ready that drains the panel/
		// LCD and MIDI TX rings), so we drain them all here; injectInterrupt/raiseIPL gate on
		// the CPU's SR mask. See mdsim.h.
		uint8_t level, vector;
		while(m_sim.needsInterruptCheck() && m_sim.takeNextInterrupt(level, vector))
		{
			injectInterrupt(vector, level);
		}

		if(m_externalIrq4Pending || m_sim.externalIrq4Asserted())
			serviceExternalIrq4();
	}

	uint32_t Microcontroller::onIllegalInstruction(const uint32_t _opcode)
	{
		(void)_opcode;
		// Return 0 so Musashi raises the standard illegal-instruction exception rather
		// than the base class asserting.
		return 0;
	}

	uint8_t Microcontroller::read8(const uint32_t _addr)
	{
		if(m_model == MachineModel::Machinedrum)
		{
			const auto offset = memorymap::g_flashLow.contains(_addr)
				? memorymap::g_flashLow.offset(_addr)
				: (memorymap::g_flashFull.contains(_addr)
					? memorymap::g_flashFull.offset(_addr) : UINT32_MAX);
			if(offset != UINT32_MAX)
				if(const auto value = m_flashCommands.read8(offset))
					return *value;
		}
		if(memorymap::g_sim.contains(_addr))		return m_sim.read8(memorymap::g_sim.offset(_addr));
		if(memorymap::g_dsp1Hdi08.contains(_addr))	return m_hdi08Dsp1.read8(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp1Hdi08.offset(_addr)));
		if(memorymap::g_dsp2Hdi08.contains(_addr))	return m_hdi08Dsp2.read8(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp2Hdi08.offset(_addr)));
		if(memorymap::isPatchRam(_addr))
		{
			// Dual-backed window, reads follow the last writer per sector.
			std::shared_lock patchLock(m_patchRamMutex);
			const auto r = patchWindowRegion(_addr);
			if(!r.data || r.offset >= r.size)	return 0;
			return r.data[r.offset];
		}
		const auto r = resolve(_addr);
		if(r.peripheral)					{ logPeripheral(_addr, 0, 1, false); return 0; }
		if(!r.data || r.offset >= r.size)	return 0;
		return r.data[r.offset];
	}

	bool Microcontroller::tryUpdatePatchBytes(const PatchByteUpdate* const _updates,
		const size_t _count)
	{
		if(!_updates || _count == 0)
			return false;
		std::unique_lock patchLock(m_patchRamMutex, std::try_to_lock);
		if(!patchLock.owns_lock())
			return false;

		// Automation-time patch edits target the running (RAM) image, never
		// flash: validate and write the RAM backing directly. They never flip
		// the window (a flash-phase machine has no automation traffic).
		for(size_t i = 0; i < _count; ++i)
		{
			if(!memorymap::isPatchRam(_updates[i].address))
				return false;
			if((_updates[i].address & 0x000fffffu) >= m_patchRam.size())
				return false;
		}
		for(size_t i = 0; i < _count; ++i)
		{
			const uint32_t ramOffset = _updates[i].address & 0x000fffffu;
			const auto& update = _updates[i];
			m_patchRam[ramOffset] = static_cast<uint8_t>(
				(m_patchRam[ramOffset] & ~update.mask)
				| (update.value & update.mask));
		}
		return true;
	}

	bool Microcontroller::tryReadPatchBytes(const uint32_t* const _addresses,
		uint8_t* const _values, const size_t _count)
	{
		if(!_addresses || !_values || _count == 0)
			return false;
		std::shared_lock patchLock(m_patchRamMutex, std::try_to_lock);
		if(!patchLock.owns_lock())
			return false;

		for(size_t i = 0; i < _count; ++i)
		{
			if(!memorymap::isPatchRam(_addresses[i]))
				return false;
			const auto region = patchWindowRegion(_addresses[i]);
			if(!region.data || region.offset >= region.size)
				return false;
			_values[i] = region.data[region.offset];
		}
		return true;
	}

	uint16_t Microcontroller::read16(const uint32_t _addr)
	{
		if(m_model == MachineModel::Machinedrum)
		{
			const auto offset = memorymap::g_flashLow.contains(_addr)
				? memorymap::g_flashLow.offset(_addr)
				: (memorymap::g_flashFull.contains(_addr)
					? memorymap::g_flashFull.offset(_addr) : UINT32_MAX);
			if(offset != UINT32_MAX)
				if(const auto value = m_flashCommands.read16(offset))
					return *value;
		}
		if(memorymap::g_sim.contains(_addr))		return m_sim.read16(memorymap::g_sim.offset(_addr));
		if(memorymap::g_dsp1Hdi08.contains(_addr))	return m_hdi08Dsp1.read16(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp1Hdi08.offset(_addr)));
		if(memorymap::g_dsp2Hdi08.contains(_addr))	return m_hdi08Dsp2.read16(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp2Hdi08.offset(_addr)));
		if(memorymap::isPatchRam(_addr))
		{
			// Dual-backed window, see read8.
			std::shared_lock patchLock(m_patchRamMutex);
			const auto r = patchWindowRegion(_addr);
			if(!r.data || (r.offset + 1) >= r.size)	return 0;
			return mc68k::memoryOps::readU16(r.data, r.offset);
		}
		const auto r = resolve(_addr);
		if(r.peripheral)						{ logPeripheral(_addr, 0, 2, false); return 0; }
		if(!r.data || (r.offset + 1) >= r.size)	return 0;
		return mc68k::memoryOps::readU16(r.data, r.offset);
	}

	void Microcontroller::write8(const uint32_t _addr, const uint8_t _val)
	{
		if(memorymap::g_sim.contains(_addr))		{ m_sim.write8(memorymap::g_sim.offset(_addr), _val); return; }
		if(memorymap::g_dsp1Hdi08.contains(_addr))	{ m_hdi08Dsp1.write8(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp1Hdi08.offset(_addr)), _val); return; }
		if(memorymap::g_dsp2Hdi08.contains(_addr))	{ m_hdi08Dsp2.write8(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp2Hdi08.offset(_addr)), _val); return; }
		if(memorymap::isPatchRam(_addr))
		{
			// Byte writes never carry flash command traffic (the bootloader
			// uses word writes); they accumulate the RAM copy and select it.
			std::unique_lock patchLock(m_patchRamMutex);
			const uint32_t ramOffset = _addr & 0x000fffffu;
			if(ramOffset < m_patchRam.size())
				m_patchRam[ramOffset] = _val;
			setPatchWindowSectorFlash(_addr, false);
			return;
		}
		const auto r = resolve(_addr);
		if(r.peripheral)								{ logPeripheral(_addr, _val, 1, true); return; }
		if(!r.writable || !r.data || r.offset >= r.size)	return;
		r.data[r.offset] = _val;
	}

	void Microcontroller::write16(const uint32_t _addr, const uint16_t _val)
	{
		if(m_model == MachineModel::Machinedrum)
		{
			const bool window = memorymap::isPatchRam(_addr);
			const auto offset = memorymap::g_flashLow.contains(_addr)
				? memorymap::g_flashLow.offset(_addr)
				: (memorymap::g_flashFull.contains(_addr)
					? memorymap::g_flashFull.offset(_addr)
					: (window ? patchWindowFlashOffset(_addr) : UINT32_MAX));
			if(offset != UINT32_MAX)
			{
				bool executed = false;
				if(const auto operation = m_flashCommands.write16(offset, _val))
				{
					std::unique_lock flashLock(m_flashMutex);
					bool changed = false;
					if(operation->type == FlashCommandDecoder::Operation::Type::ProgramWord
						&& operation->offset + 1 < m_flashData.size())
					{
						// NOR programming can only clear bits; erasing restores them.
						m_flashData[operation->offset] &= static_cast<uint8_t>(operation->value >> 8);
						m_flashData[operation->offset + 1] &= static_cast<uint8_t>(operation->value);
						changed = true;
					}
					else if(operation->type == FlashCommandDecoder::Operation::Type::EraseSector)
					{
						const auto begin = FlashCommandDecoder::eraseSectorBegin(
							operation->offset);
						const auto end = std::min<uint32_t>(begin
							+ FlashCommandDecoder::eraseSectorSize(operation->offset),
							static_cast<uint32_t>(m_flashData.size()));
						if(begin < end)
						{
							std::fill(m_flashData.begin() + begin, m_flashData.begin() + end,
								uint8_t{0xff});
							changed = true;
						}
					}
					if(changed)
					{
						m_flashDirty = true;
						m_lastFlashWriteCycle = getCycles();
						m_immPageAddress = 0xffffffffu;
						m_immPageData = nullptr;
						if(m_flashOperationObserver) m_flashOperationObserver(*operation, getCycles());
					}
					executed = true;
				}
				// The 0x00100000/0x00700000 patch window aliases flash offsets
				// 0x100000-0x1fffff. Bulk programming may target those cells
				// through the low/full flash windows instead of the patch
				// window; without mirroring, a later patch-window read would
				// observe the stale RAM copy instead of the programmed flash
				// (the post-upgrade DSP/OS checksum reads hit exactly this).
				// Keep the dual backing coherent the same way the patch-window
				// path does: every bus word lands in the RAM copy, and an
				// executed flash command selects flash for its sector.
				const uint32_t mirrorAddr = !window
					&& offset >= memorymap::g_patchBootstrap.begin
					&& offset < memorymap::g_patchBootstrap.end
					? offset : UINT32_MAX;
				if(window || mirrorAddr != UINT32_MAX)
				{
					// Flash commands select their sector's flash, plain
					// writes accumulate the RAM copy (see patchWindowRegion).
					std::unique_lock patchLock(m_patchRamMutex);
					const uint32_t ramOffset = (window ? _addr : mirrorAddr) & 0x000fffffu;
					if(ramOffset + 1 < m_patchRam.size())
						mc68k::memoryOps::writeU16(m_patchRam.data(), ramOffset, _val);
					setPatchWindowSectorFlash(window ? _addr : mirrorAddr, executed);
				}
				return;
			}
		}
		if(memorymap::g_sim.contains(_addr))		{ m_sim.write16(memorymap::g_sim.offset(_addr), _val); return; }
		if(memorymap::g_dsp1Hdi08.contains(_addr))	{ m_hdi08Dsp1.write16(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp1Hdi08.offset(_addr)), _val); return; }
		if(memorymap::g_dsp2Hdi08.contains(_addr))	{ m_hdi08Dsp2.write16(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp2Hdi08.offset(_addr)), _val); return; }
		if(m_monomachineFlash && (memorymap::g_flashFull.contains(_addr)
			|| memorymap::g_flashLow.contains(_addr) || memorymap::isPatchRam(_addr)))
		{
			const bool window = memorymap::isPatchRam(_addr);
			const auto offset = memorymap::g_flashFull.contains(_addr)
				? memorymap::g_flashFull.offset(_addr)
				: (memorymap::g_flashLow.contains(_addr)
					? memorymap::g_flashLow.offset(_addr)
					: patchWindowFlashOffset(_addr));
			bool executed = false;
			{
				std::unique_lock flashLock(m_flashMutex);
				executed = m_monomachineFlash->write(offset, _val);
				if(executed)
				{
					m_flashDirty = true;
					m_lastFlashWriteCycle = getCycles();
					m_immPageAddress = 0xffffffffu;
					m_immPageData = nullptr;
				}
			}
			// Same mirroring as the MD path: bulk programming through the
			// low/full windows must stay coherent with the patch-window read
			// source for aliased offsets.
			const uint32_t mirrorAddr = !window
				&& offset >= memorymap::g_patchBootstrap.begin
				&& offset < memorymap::g_patchBootstrap.end
				? offset : UINT32_MAX;
			if(window || mirrorAddr != UINT32_MAX)
			{
				// Same dual-backing contract as the MD path above: commands
				// select their sector's flash, plain data accumulates the RAM copy.
				std::unique_lock patchLock(m_patchRamMutex);
				const uint32_t ramOffset = (window ? _addr : mirrorAddr) & 0x000fffffu;
				if(ramOffset + 1 < m_patchRam.size())
					mc68k::memoryOps::writeU16(m_patchRam.data(), ramOffset, _val);
				setPatchWindowSectorFlash(window ? _addr : mirrorAddr, executed);
			}
			else if(executed)
			{
				m_flashDirty = true;
				m_lastFlashWriteCycle = getCycles();
				m_immPageAddress = 0xffffffffu;
				m_immPageData = nullptr;
			}
			return;
		}
		const auto r = resolve(_addr);
		if(r.peripheral)										{ logPeripheral(_addr, _val, 2, true); return; }
		if(!r.writable || !r.data || (r.offset + 1) >= r.size)	return;
		mc68k::memoryOps::writeU16(r.data, r.offset, _val);
	}

	uint16_t Microcontroller::readImm16(const uint32_t _addr)
	{
		// Instruction fetch: always from ROM/RAM, never a peripheral - do not log.
		static constexpr uint32_t pageSize = 4096;
		static constexpr uint32_t pageMask = pageSize - 1;
		const uint32_t pageAddress = _addr & ~pageMask;
		const uint32_t pageOffset = _addr & pageMask;
		if(pageOffset + 1 < pageSize && pageAddress == m_immPageAddress)
			return mc68k::memoryOps::readU16(m_immPageData, pageOffset);

		// The patch window resolves against its current read source (flash
		// image or RAM copy); everything else keeps the static mapping.
		const auto r = memorymap::isPatchRam(_addr) ? patchWindowRegion(_addr) : resolve(_addr);
		if(r.peripheral || !r.data || (r.offset + 1) >= r.size)	return 0;

		// All normal backing windows are page-aligned, but keep the cache
		// conditional so an unusual future mapping retains the resolve() result.
		if(pageOffset + 1 < pageSize && r.offset >= pageOffset
			&& (r.offset & pageMask) == pageOffset
			&& r.offset - pageOffset + pageSize <= r.size)
		{
			m_immPageAddress = pageAddress;
			m_immPageData = r.data + r.offset - pageOffset;
			return mc68k::memoryOps::readU16(m_immPageData, pageOffset);
		}
		return mc68k::memoryOps::readU16(r.data, r.offset);
	}

	uint32_t Microcontroller::getResetSP()
	{
		// 680x0/ColdFire load the initial SP from the long at address 0 at reset.
		return (static_cast<uint32_t>(readImm16(0)) << 16) | readImm16(2);
	}

	uint32_t Microcontroller::getResetPC()
	{
		// ...and the initial PC from the long at address 4.
		return (static_cast<uint32_t>(readImm16(4)) << 16) | readImm16(6);
	}
}
