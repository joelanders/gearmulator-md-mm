#include "mdmc.h"

#include "mdfrontpanel.h"
#include "mdmemorymap.h"
#include "mdrom.h"

#include "mc68k/memoryOps.h"
#include "mc68k/Musashi/m68k.h"
#include "mc68k/cpuState.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

// Provide the Musashi memory-access callbacks (m68k_read_memory_*, _pcrelative_*, etc.)
// for this microcontroller. Exactly one TU per synth includes this - cf. n2xmc.cpp / xtUc.cpp.
#define MC68K_CLASS md::Microcontroller
#include "mc68k/musashiEntry.h"

namespace md
{
	namespace
	{
		// The SFX-60 MKII stores user DigiPRO waves in the uniform-sector portion of
		// its 8 MiB AMD-compatible flash. The firmware erases it in 64 KiB sectors.
		class MonomachineFlash final : public hwLib::Am29f
		{
		public:
			MonomachineFlash(uint8_t* _data, const size_t _size)
				: Am29f(_data, _size, false, true), m_size(_size)
			{
			}

			bool eraseSector(const uint32_t _addr) const override
			{
				constexpr size_t sectorSize = 64 * 1024;
				if((_addr % sectorSize) != 0 || _addr > m_size || sectorSize > m_size - _addr)
					return false;
				return Am29f::eraseSector(_addr, sectorSize / 1024);
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
		const std::vector<uint8_t>& _initialMainRam,
		const std::vector<uint8_t>& _initialUserFlash,
		const std::vector<uint8_t>& _initialPlusDrive,
		const bool _plusDriveEnabled)
		: Mc68k(M68K_CPU_TYPE_MCF5206E)
		, m_model(_model)
		, m_rom(_rom)
		, m_flashData(_rom.data())
		, m_patchRam(memorymap::g_patchBootstrap.size(), 0)
		, m_mainRam(memorymap::g_mainRam.size(), 0)
		, m_loaderRam(memorymap::g_loaderRam.size(), 0)
		, m_internalSram(memorymap::g_internalSram.size(), 0)
	{
		if(m_model == MachineModel::Machinedrum
			&& _initialUserFlash.size() == m_flashData.size())
			m_flashData = _initialUserFlash;
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
			m_flash = std::make_unique<MonomachineFlash>(m_flashData.data(), m_flashData.size());

		// Report the MKII board profile used by both supported targets.
		m_sim.setMk2PortAInvertedLoopback(true);
		m_sim.setPlusDriveEnabled(
			m_model == MachineModel::Machinedrum && _plusDriveEnabled);
		if(m_model == MachineModel::Machinedrum)
			m_sim.getPlusDrive().replaceStorage(_initialPlusDrive);

		// The panel controller is not part of the emulator. Reproduce the public
		// MAME driver's UART startup handshake here.
		m_sim.setTransmitCallback(Sim::g_uartPanel, [this](const uint8_t _b) { onPanelTransmit(_b); });
		m_sim.setTransmitCallback(Sim::g_uartMidi, [this](const uint8_t _b)
		{
			{
				const std::scoped_lock lock(m_midiTxMutex);
				auto& producer = m_midiTxBuffers[m_midiTxProducerIndex];
				if(producer.size < producer.bytes.size())
					producer.bytes[producer.size++] = _b;
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
		if(_initialMainRam.size() <= m_mainRam.size())
			std::copy(_initialMainRam.begin(), _initialMainRam.end(), m_mainRam.begin());
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

	std::vector<uint8_t> Microcontroller::copyFlashData() const
	{
		std::shared_lock lock(m_flashMutex);
		return m_flashData;
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
		return StateImagePublishResult::Published;
	}

	void Microcontroller::prepareFirmwareUpdateBoot(const uint32_t _factoryFlashAddress)
	{
		// State established by the MCF5206E first-stage loader before the main OS
		// handoff. ACR0/1 remain at reset (zero).
		auto* const cpu = getCpuState();
		cpu->vbr = 0x01000000;
		cpu->cacr = 0x81000503;
		cpu->cf_rambar = 0x01000001;
		cpu->cf_mbar = 0x00300001;
		m68k_set_reg(cpu, M68K_REG_D1, 1);
		if(m_model == MachineModel::Machinedrum)
		{
			m68k_set_reg(cpu, M68K_REG_D2, 0);
			m68k_set_reg(cpu, M68K_REG_A2, _factoryFlashAddress - 1);
		}
		else
		{
			m68k_set_reg(cpu, M68K_REG_D2, _factoryFlashAddress - 0x10);
			m68k_set_reg(cpu, M68K_REG_A2, _factoryFlashAddress);
		}
		// The physical first stage completes the panel-present handshake before it
		// enters the main OS. Direct update boot skips that UART exchange, so publish
		// the same bridge state and route subsequent bytes to the panel decoder.
		m_panelProbeIndex = 0;
		m_mmPanelProbeIndex = 0;
		m_mmPanelHandshakeDone = true;
		m_panelDisplayReady = true;
	}

	void Microcontroller::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
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
			m_midiTxParser.write(drain.bytes[i]);
		m_midiTxParser.getEvents(_midiOut);
		if(discontinuity)
			m_midiTxParser.discardPartialMessage();
		drain.size = 0;
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
		// MAME-compatible Monomachine panel handshake.
		if(m_model == MachineModel::Monomachine)
		{
			// The exchange consists of an autobaud reply, a startup probe, and a
			// compact panel descriptor.
			constexpr uint8_t cfg = 0x01;
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
					m_sim.queueRx(Sim::g_uartPanel, cfg);
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

		// Match the Machinedrum panel probe implemented by the MAME Elektron driver.
		static constexpr uint8_t probe[] =
			{ 0x20, 0xff, 0x21, 0xff, 0x22, 0xff, 0x23, 0xff, 0x24, 0xff, 0x25, 0xff, 0x30, 0x00 };

		if(_byte == probe[m_panelProbeIndex])
		{
			if(++m_panelProbeIndex == sizeof(probe))
			{
				m_panelProbeIndex = 0;
				// Startup reply, matching MAME panel_send_startup_reply's default: ready
				// signature 0x24, then the panel "startup flags" byte (MAME's PANEL config
				// ioport, whose default is 0x00 - a DIP-style panel-variant selector), then
				// the model/status byte 0x00. 0x00 is the correct default, not a placeholder.
				m_sim.queueRx(Sim::g_uartPanel, 0x24);	// startup ready signature
				m_sim.queueRx(Sim::g_uartPanel, 0x00);	// startup flags (PANEL config default)
				m_sim.queueRx(Sim::g_uartPanel, 0x00);	// model / status

				// The panel is now "present"; enable the periodic display-ready semaphore
				// post (MAME: panel_send_startup_reply -> panel_display_ready).
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
		const auto cycles = execInstruction();
		advanceAfterCpu(cycles);
		return cycles;
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
		while(m_sim.takeNextInterrupt(level, vector))
		{
			injectInterrupt(vector, level);
		}

		serviceExternalIrq4();

		// Periodically service the public MAME driver's panel-ready notification.
		if(m_panelDisplayReady && (++m_panelDisplayReadyDivider & 0x3fff) == 0)
			panelDisplayReadyPost();
	}

	uint32_t Microcontroller::readMem32(const uint32_t _addr)
	{
		return (static_cast<uint32_t>(read16(_addr)) << 16) | read16(_addr + 2);
	}

	void Microcontroller::writeMem32(const uint32_t _addr, const uint32_t _value)
	{
		write16(_addr,     static_cast<uint16_t>(_value >> 16));
		write16(_addr + 2, static_cast<uint16_t>(_value & 0xffff));
	}

	void Microcontroller::panelDisplayReadyPost()
	{
		// Panel-ready notification compatible with MAME's Elektron driver.
		constexpr uint32_t g_semaphore       = 0x002899e8;
		constexpr uint32_t g_semaphoreSlot   = 0x0028d714;
		constexpr uint32_t g_highestReadyList= 0x01001dc4;
		constexpr uint32_t g_sramBase        = 0x01000000;
		constexpr uint32_t g_sramEnd         = 0x01010000;

		auto inSram = [](const uint32_t _a) { return !(_a & 3) && _a >= g_sramBase && _a <= (g_sramEnd - 4); };

		// The public driver validates the notification slot before updating it.
		if(readMem32(g_semaphoreSlot) != g_semaphore)
			return;

		const uint32_t count  = readMem32(g_semaphore);
		const uint32_t waiter = readMem32(g_semaphore + 4);

		if(waiter == 0)
		{
			// Record a pending notification when no receiver is waiting.
			if(count == 0)
				writeMem32(g_semaphore, 1);
			return;
		}

		if(!inSram(waiter))
			return;

		const uint32_t readyList = readMem32(waiter + 8);
		if(!inSram(readyList))
			return;

		const uint32_t head = readMem32(readyList);
		uint32_t headNext = 0;
		if(head != 0)
		{
			if(!inSram(head))
				return;
			headNext = readMem32(head);
			if(!inSram(headNext))
				return;
		}

		// Complete the public driver's bounded panel-ready update.
		writeMem32(g_semaphore, count);
		writeMem32(g_semaphore + 4, 0);

		const uint32_t highest = readMem32(g_highestReadyList);
		if(readyList > highest)
			writeMem32(g_highestReadyList, readyList);

		if(head == 0)
		{
			writeMem32(readyList, waiter);
			writeMem32(waiter, waiter);
			writeMem32(waiter + 4, waiter);
		}
		else
		{
			writeMem32(waiter + 4, head);
			writeMem32(waiter, headNext);
			writeMem32(headNext + 4, waiter);
			writeMem32(readyList, waiter);
		}
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
		const bool patchRam = memorymap::isPatchRam(_addr);
		std::shared_lock patchLock(m_patchRamMutex, std::defer_lock);
		if(patchRam)
			patchLock.lock();
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

		for(size_t i = 0; i < _count; ++i)
		{
			if(!memorymap::isPatchRam(_updates[i].address))
				return false;
			const auto region = resolve(_updates[i].address);
			if(!region.writable || !region.data || region.offset >= region.size)
				return false;
		}
		for(size_t i = 0; i < _count; ++i)
		{
			const auto region = resolve(_updates[i].address);
			const auto& update = _updates[i];
			region.data[region.offset] = static_cast<uint8_t>(
				(region.data[region.offset] & ~update.mask)
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
			const auto region = resolve(_addresses[i]);
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
		const bool patchRam = memorymap::isPatchRam(_addr);
		std::shared_lock patchLock(m_patchRamMutex, std::defer_lock);
		if(patchRam)
			patchLock.lock();
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
		const bool patchRam = memorymap::isPatchRam(_addr);
		std::unique_lock patchLock(m_patchRamMutex, std::defer_lock);
		if(patchRam)
			patchLock.lock();
		const auto r = resolve(_addr);
		if(r.peripheral)								{ logPeripheral(_addr, _val, 1, true); return; }
		if(!r.writable || !r.data || r.offset >= r.size)	return;
		r.data[r.offset] = _val;
	}

	void Microcontroller::write16(const uint32_t _addr, const uint16_t _val)
	{
		if(m_model == MachineModel::Machinedrum)
		{
			const auto offset = memorymap::g_flashLow.contains(_addr)
				? memorymap::g_flashLow.offset(_addr)
				: (memorymap::g_flashFull.contains(_addr)
					? memorymap::g_flashFull.offset(_addr) : UINT32_MAX);
			if(offset != UINT32_MAX)
			{
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
						const auto begin = operation->offset
							& ~(FlashCommandDecoder::g_sectorSize - 1);
						const auto end = std::min<uint32_t>(begin
							+ FlashCommandDecoder::g_sectorSize,
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
					}
				}
				return;
			}
		}
		if(memorymap::g_sim.contains(_addr))		{ m_sim.write16(memorymap::g_sim.offset(_addr), _val); return; }
		if(memorymap::g_dsp1Hdi08.contains(_addr))	{ m_hdi08Dsp1.write16(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp1Hdi08.offset(_addr)), _val); return; }
		if(memorymap::g_dsp2Hdi08.contains(_addr))	{ m_hdi08Dsp2.write16(static_cast<mc68k::PeriphAddress>(memorymap::g_dsp2Hdi08.offset(_addr)), _val); return; }
		if(m_flash && memorymap::g_flashFull.contains(_addr))
		{
			std::unique_lock flashLock(m_flashMutex);
			m_flash->write(memorymap::g_flashFull.offset(_addr), _val);
			m_immPageAddress = 0xffffffffu;
			m_immPageData = nullptr;
			return;
		}
		if(m_flash && memorymap::g_flashLow.contains(_addr))
		{
			std::unique_lock flashLock(m_flashMutex);
			m_flash->write(memorymap::g_flashLow.offset(_addr), _val);
			m_immPageAddress = 0xffffffffu;
			m_immPageData = nullptr;
			return;
		}
		const bool patchRam = memorymap::isPatchRam(_addr);
		std::unique_lock patchLock(m_patchRamMutex, std::defer_lock);
		if(patchRam)
			patchLock.lock();
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

		const auto r = resolve(_addr);
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
