#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace md
{
	// Set machine-time coordinates without running billions of instructions.
	// Publication, callbacks, latch reads and IRQ wiring use the real runtime.
	struct HostRxFirmwareTestAccess
	{
		static void origin(Hardware& hw, unsigned index, double frame, uint64_t cycle)
		{
			hw.m_schedDspOriginLatched[index] = true;
			hw.m_schedDspOriginFrame[index] = frame;
			hw.m_schedDspOriginCycles[index] = cycle;
		}
		static void now(Hardware& hw, uint64_t cycle) { hw.m_schedUcCyclesDone = cycle; }
		static void clearWake(Hardware& hw) { hw.m_schedulerHostPumpDirty.store(false); }
		static void pump(Hardware& hw) { hw.pumpDsp2HostRequest(); }
	};
}

namespace
{
	using Access = md::HostRxFirmwareTestAccess;
	void require(bool condition, const char* message)
	{
		if(!condition) throw std::runtime_error(message);
	}

	bool irq(md::Hardware& hw)
	{
		uint8_t level = 0, vector = 0;
		const bool raised = hw.getUC().getSim().getExternalIrq4(level, vector);
		require(!raised || (level == 4 && vector == 28), "wrong IRQ level/vector");
		return raised;
	}

	uint32_t readWord(mc68k::Hdi08& port, bool littleEndian)
	{
		if(littleEndian)
		{
			const auto high = port.read8(mc68k::PeriphAddress::HdiTXL);
			const auto middle = port.read8(mc68k::PeriphAddress::HdiTXM);
			const auto low = port.read8(mc68k::PeriphAddress::HdiTXH);
			return (uint32_t(high) << 16) | (uint32_t(middle) << 8) | low;
		}
		const auto high = port.read16(mc68k::PeriphAddress::HdiUnused4);
		const auto low = port.read16(mc68k::PeriphAddress::HdiTXM);
		return (uint32_t(high) << 16) | low;
	}

	std::unique_ptr<md::Hardware> machine(const std::vector<uint8_t>& rom)
	{
		auto hw = std::make_unique<md::Hardware>(rom, "host-receive-test",
			md::MachineModel::Monomachine);
		require(hw->isValid(), "invalid test hardware");
		// One-cycle MOVEQ instructions make the publication boundary exact.
		// No firmware routine is run during this integration test.
		for(unsigned i = 0; i < 16; ++i) hw->getUC().write16(0x200000 + 2 * i, 0x7000);
		hw->getUC().setPC(0x200000);
		auto& sim = hw->getUC().getSim();
		sim.write16(md::Sim::g_imr, 0);
		sim.write8(md::Sim::g_icrExtIrq4, 4 << 2);
		return hw;
	}

	void conversion(const std::vector<uint8_t>& rom)
	{
		auto hw = machine(rom);
		Access::now(*hw, 123);
		for(unsigned index = 0; index < 2; ++index)
		{
			require(hw->hostRxReadyCycle(index, 500) == 123, "bootstrap was deferred");
			Access::origin(*hw, index, 0, 100);
			require(hw->hostRxReadyCycle(index, 99) == 123, "pre-origin timestamp underflowed");
			// Integer rational oracle for the configured 40 MHz / 101.6064 MHz
			// clocks; deliberately independent of production frame/double arithmetic.
			for(uint64_t delta = 0; delta < 1016064; delta += 7)
			{
				const auto expected = (delta * 6250 + 15876 - 1) / 15876;
				require(hw->hostRxReadyCycle(index, 100 + delta) == expected,
					"DSP-to-host conversion rounded to the wrong cycle");
			}
			Access::origin(*hw, index, 44100.0 * 86400, uint64_t{1} << 40);
			const auto expected = uint64_t{40000000} * 86400 + 1;
			require(hw->hostRxReadyCycle(index, (uint64_t{1} << 40) + 1) == expected,
				"24-hour origin or 64-bit DSP timestamp was truncated");
		}
		std::cout << "DSP clock conversion: both origins, rational sweep and 24-hour timestamps passed\n";
	}

	void delivery(const std::vector<uint8_t>& rom, unsigned index, bool littleEndian)
	{
		auto hw = machine(rom);
		auto& dsp = index ? hw->getDspProducer() : hw->getDspMixer();
		auto& host = index ? hw->getUC().getHdi08Dsp2() : hw->getUC().getHdi08Dsp1();
		// Allow the real pump to see both DSPs as runnable. Their programs are
		// idle; the test writes through the real DSP HOTX and its installed callback.
		hw->getDspProducer().onDspBootFinished();
		hw->getDspMixer().onDspBootFinished();
		Access::origin(*hw, index, 1, dsp.dsp().getCycles());
		Access::now(*hw, 900);
		host.icr(mc68k::Hdi08::Rreq | (littleEndian ? mc68k::Hdi08::Hlend : 0));
		dsp.hdi08().writeTX(0); // A legitimate voice-zero notification.
		require(dsp.hasDeferredHostRx() && host.hostRxWordsAvailable() == 0,
			"future first word became readable");
		dsp.dsp().fastForward(0, 8);
		dsp.hdi08().writeTX(0x41f);
		require(dsp.hdi08().hasTX() && dsp.hostTxBacklog() == 2,
			"second word did not remain in native HOTX");
		Access::pump(*hw);
		require(!irq(*hw), "future data raised HREQ");
		Access::clearWake(*hw);
		Access::now(*hw, 907);
		hw->processUC();
		require(host.hostRxWordsAvailable() == 0 && !irq(*hw),
			"word arrived before its 908-cycle deadline");
		require(hw->hostCurrentCycle() == 908, "synthetic CPU did not advance one cycle");
		// No new DSP/peripheral edge: CPU time alone must wake publication.
		hw->processUC();
		require(host.hostRxWordsAvailable() == 1 && !dsp.hasDeferredHostRx(),
			"deferred word stalled after the dirty wake was cleared");
		require(irq(*hw) == (index == 1), "HREQ wired to the wrong DSP");
		require(dsp.hdi08().hasTX(), "readable latch admitted another word");
		require(readWord(host, littleEndian) == 0, "voice-zero header was lost");
		require(dsp.hasDeferredHostRx() && host.hostRxWordsAvailable() == 0,
			"second word lost its original 911-cycle deadline");
		Access::pump(*hw);
		require(!irq(*hw), "HREQ stayed asserted for a future second word");
		Access::now(*hw, 911);
		Access::clearWake(*hw);
		Access::pump(*hw);
		require(host.hostRxWordsAvailable() == 1, "clock word never became readable");
		require(irq(*hw) == (index == 1), "clock-word HREQ missing");
		// Turning receive requests off must lower IRQ while retaining readable data.
		host.icr(littleEndian ? mc68k::Hdi08::Hlend : 0);
		Access::pump(*hw);
		require(!irq(*hw) && host.hostRxWordsAvailable() == 1, "RREQ cleared data or kept IRQ high");
		host.icr(mc68k::Hdi08::Rreq | (littleEndian ? mc68k::Hdi08::Hlend : 0));
		Access::pump(*hw);
		require(irq(*hw) == (index == 1), "enabling RREQ missed an already-readable word");
		require(readWord(host, littleEndian) == 0x41f, "clock word reordered or lost");
		Access::pump(*hw);
		require(host.hostRxWordsAvailable() == 0 && !dsp.hasDeferredHostRx()
			&& !dsp.hdi08().hasTX() && !irq(*hw), "delivery duplicated a word or left IRQ high");
		std::cout << "Real host bridge: DSP " << index << ", endian " << littleEndian << " passed\n";
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path || !*path)
	{
		std::cout << "Set GEARMULATOR_MM_FIRMWARE_BIN to the supported MM image\n";
		return 77;
	}
	try
	{
		std::vector<uint8_t> rom;
		require(baseLib::filesystem::readFile(rom, path), "could not read firmware");
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Monomachine),
			"unsupported firmware");
		conversion(rom);
		for(unsigned index = 0; index < 2; ++index)
			for(bool littleEndian : {false, true}) delivery(rom, index, littleEndian);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
