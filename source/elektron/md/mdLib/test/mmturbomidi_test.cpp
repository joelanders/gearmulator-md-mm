#include "mdLib/mdhardware.h"
#include "mdLib/mdstate.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	std::vector<uint8_t> load(const char* const _path)
	{
		std::ifstream input(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}

	void settle(md::Hardware& _hardware, const size_t _steps = 100)
	{
		for(size_t i = 0; i < _steps; ++i)
			_hardware.advance(64);
	}

	void pulse(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(md::MachineModel::Monomachine, _control);
		if(!packet)
			return;
		_hardware.sendPanelEvent(packet->row, packet->mask);
		settle(_hardware, 8);
		_hardware.sendPanelEvent(packet->row, 0);
		settle(_hardware);
	}

	void chord(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto function = md::panelPacket(md::MachineModel::Monomachine,
			md::PanelControl::Function);
		const auto target = md::panelPacket(md::MachineModel::Monomachine, _control);
		if(!function || !target)
			return;
		md::PanelRowState rows;
		auto packet = rows.press(*function);
		_hardware.sendPanelEvent(packet.row, packet.mask);
		settle(_hardware, 4);
		packet = rows.press(*target);
		_hardware.sendPanelEvent(packet.row, packet.mask);
		settle(_hardware, 8);
		packet = rows.release(*target);
		_hardware.sendPanelEvent(packet.row, packet.mask);
		settle(_hardware, 4);
		packet = rows.release(*function);
		_hardware.sendPanelEvent(packet.row, packet.mask);
		settle(_hardware);
	}

	void writePanel(const md::Hardware& _hardware, const std::string& _path)
	{
		const auto panel = _hardware.getFrontPanelSnapshot();
		std::ofstream output(_path, std::ios::binary);
		output << "P5\n" << md::FrontPanel::g_lcdWidth << ' '
			<< md::FrontPanel::g_lcdHeight << "\n255\n";
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				output.put(panel.getLcdPixel(x, y) ? '\0' : '\xff');
	}

	uint64_t panelFingerprint(const md::Hardware& _hardware)
	{
		const auto panel = _hardware.getFrontPanelSnapshot();
		uint64_t result = 14695981039346656037ull;
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
			{
				result ^= panel.getLcdPixel(x, y) ? 1u : 0u;
				result *= 1099511628211ull;
			}
		return result;
	}

	bool enterDigiProReceive(md::Hardware& _hardware)
	{
		const auto* const prefix = std::getenv("MM_PANEL_PROBE_PREFIX");
		const auto save = [&_hardware, prefix](const int _index)
		{
			if(prefix && *prefix)
				writePanel(_hardware,
					std::string(prefix) + std::to_string(_index) + ".pgm");
		};
		save(0);
		chord(_hardware, md::PanelControl::Kit);
		save(1);
		pulse(_hardware, md::PanelControl::Enter);
		save(2);
		pulse(_hardware, md::PanelControl::Down);
		save(3);
		pulse(_hardware, md::PanelControl::Down);
		save(4);
		pulse(_hardware, md::PanelControl::Right);
		save(5);
		pulse(_hardware, md::PanelControl::Down);
		save(6);
		pulse(_hardware, md::PanelControl::Down);
		save(7);
		pulse(_hardware, md::PanelControl::Enter);
		save(8);
		pulse(_hardware, md::PanelControl::Right);
		save(9);
		const auto beforeWaiting = panelFingerprint(_hardware);
		pulse(_hardware, md::PanelControl::Enter);
		save(10);
		return panelFingerprint(_hardware) != beforeWaiting;
	}

}

int main(const int _argc, char** _argv)
{
	if(_argc != 4)
	{
		std::fprintf(stderr,
			"usage: mmturbomidi_test <monomachine-rom> <factory-patch-ram> <sysex-file>\n");
		return 2;
	}

	const auto rom = load(_argv[1]);
	const auto patchRam = load(_argv[2]);
	auto bytes = load(_argv[3]);
	// Match the shipping plug-in path: each rack worker owns a deterministic
	// scheduler-backed machine, with the fidelity/catch-up options enabled.
	md::Hardware hardware(rom, _argv[1], md::MachineModel::Monomachine, patchRam);
	if(!hardware.isValid() || patchRam.size() != 0x100000 || bytes.empty())
		return 2;

	const auto bootDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
	while(!hardware.isAudioReady() ||
		hardware.getFrontPanelSnapshot().countLitPixels() <= 2000)
	{
		hardware.advance(64);
		if(std::chrono::steady_clock::now() >= bootDeadline)
		{
			std::fprintf(stderr,
				"[TurboMIDI MM test] boot timeout: audio=%s pixels=%zu ucPC=%08x\n",
				hardware.isAudioReady() ? "yes" : "no",
				static_cast<size_t>(hardware.getFrontPanelSnapshot().countLitPixels()),
				hardware.getUC().getPC());
			return 1;
		}
	}
	if(bytes.size() > 6 && bytes[6] == 0x5d)
	{
		const auto end = std::find(bytes.begin(), bytes.end(), uint8_t{0xf7});
		if(end == bytes.end())
			return 2;
		const auto flashAtMainScreen = hardware.copyUserFlash();
		auto boundaryProbe = md::prepareMidiSysexTransfer(
			std::vector<uint8_t>(bytes.begin(), end + 1));
		if(!boundaryProbe || !hardware.startMidiSysexTransfer(*boundaryProbe))
			return 1;
		const auto boundaryDeadline = std::chrono::steady_clock::now()
			+ std::chrono::seconds(30);
		for(;;)
		{
			hardware.advance(64);
			if(hardware.getMidiSysexTransferProgress().state
				== md::MidiSysexTransferState::Complete)
				break;
			if(std::chrono::steady_clock::now() >= boundaryDeadline)
			{
				std::fputs("[TurboMIDI MM test] boundary probe timed out\n", stderr);
				return 1;
			}
		}
		settle(hardware, 700);
		if(hardware.copyUserFlash() != flashAtMainScreen)
		{
			std::fputs("[TurboMIDI MM test] sender bypassed the receive screen\n", stderr);
			return 1;
		}
		std::puts("[TurboMIDI MM test] boundary guard: main-screen send left flash unchanged");
	}
	if(!enterDigiProReceive(hardware))
	{
		std::fputs("[TurboMIDI MM test] panel did not enter DigiPRO WAITING screen\n",
			stderr);
		return 1;
	}
	if(const auto* const prefix = std::getenv("MM_PANEL_PROBE_PREFIX");
		prefix && *prefix)
		return 0;
	const auto flashBefore = hardware.copyUserFlash();
	auto prepared = md::prepareMidiSysexTransfer(std::move(bytes));
	if(!prepared || !hardware.startMidiSysexTransfer(*prepared))
		return 1;

	const auto start = std::chrono::steady_clock::now();
	uint8_t payloadSpeed = 1;
	for(;;)
	{
		hardware.advance(64);
		const auto progress = hardware.getMidiSysexTransferProgress();
		if(progress.state == md::MidiSysexTransferState::Sending)
			payloadSpeed = progress.speedCode;
		if(progress.state == md::MidiSysexTransferState::Complete)
		{
			// Completion means the UART boundary drained, not that the firmware has
			// stored anything. Let its parser consume the tail, then perform the same
			// externally observable EXIT/NO action required on the hardware.
			settle(hardware, 700);
			pulse(hardware, md::PanelControl::Exit);
			const auto storeDeadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(60);
			std::vector<uint8_t> flashAfter = flashBefore;
			bool flashChanged = false;
			size_t stableObservations = 0;
			do
			{
				settle(hardware, 200);
				auto observed = hardware.copyUserFlash();
				if(observed != flashAfter)
				{
					flashChanged = true;
					stableObservations = 0;
					flashAfter = std::move(observed);
				}
				else if(flashChanged)
					++stableObservations;
			} while(stableObservations < 10
				&& std::chrono::steady_clock::now() < storeDeadline);
			size_t changedFlashBytes = 0;
			if(flashBefore.size() == flashAfter.size())
				for(size_t i = 0; i < flashBefore.size(); ++i)
					changedFlashBytes += flashBefore[i] != flashAfter[i] ? 1u : 0u;
			std::vector<uint8_t> encodedState;
			const bool stateEncoded = md::encodeState(encodedState,
				hardware.copyPatchRam(), md::MachineModel::Monomachine,
				synthLib::StateTypeGlobal, flashAfter);
			std::vector<uint8_t> restoredPatchRam;
			std::vector<uint8_t> restoredFlash;
			const bool stateRoundTrip = stateEncoded && md::decodeState(
				restoredPatchRam, restoredFlash, encodedState,
				md::MachineModel::Monomachine, synthLib::StateTypeGlobal)
				&& restoredFlash == flashAfter;
			const auto elapsed = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - start).count();
			std::printf("[TurboMIDI MM test] panel-driven: bytes=%zu "
				"payload=%sx flashChanged=%zu stateRoundTrip=%s wall=%.3fs\n",
				progress.total, md::midiTurboSpeedLabel(payloadSpeed),
				changedFlashBytes, stateRoundTrip ? "yes" : "no", elapsed);
			return payloadSpeed > 1 && changedFlashBytes > 0
				&& stableObservations >= 10 && stateRoundTrip ? 0 : 1;
		}
		if(std::chrono::steady_clock::now() - start > std::chrono::seconds(90))
		{
			std::fprintf(stderr,
				"[TurboMIDI MM test] transfer timed out: progress=%u sent=%zu/%zu "
				"ucPC=%08x\n",
				static_cast<unsigned>(progress.state), progress.sent, progress.total,
				hardware.getUC().getPC());
			return 1;
		}
	}
}
