#include "mdLib/mdhardware.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace
{
	std::vector<uint8_t> load(const char* const _path)
	{
		std::ifstream input(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>()};
	}

	size_t changedBytes(const std::vector<uint8_t>& _before,
		const std::vector<uint8_t>& _after)
	{
		if(_before.size() != _after.size())
			return std::max(_before.size(), _after.size());
		size_t changed = 0;
		for(size_t i = 0; i < _before.size(); ++i)
			changed += _before[i] != _after[i] ? 1u : 0u;
		return changed;
	}

	const char* validationName(const md::MidiSysexStreamValidation _validation)
	{
		switch(_validation)
		{
		case md::MidiSysexStreamValidation::Valid: return "valid";
		case md::MidiSysexStreamValidation::Empty: return "empty";
		case md::MidiSysexStreamValidation::TooLarge: return "too large";
		case md::MidiSysexStreamValidation::InvalidFraming: return "invalid framing";
		case md::MidiSysexStreamValidation::InvalidDataByte: return "invalid data byte";
		case md::MidiSysexStreamValidation::ChecksumMismatch: return "checksum/length mismatch";
		case md::MidiSysexStreamValidation::UnsupportedMessage: return "unsupported message";
		case md::MidiSysexStreamValidation::WrongModel: return "wrong model";
		case md::MidiSysexStreamValidation::FirmwareUpdate: return "firmware update";
		}
		return "unknown";
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

	void writePanelProbe(const md::Hardware& _hardware, const char* _suffix);

	void enterMmGeneralSysexReceive(md::Hardware& _hardware)
	{
		writePanelProbe(_hardware, "-0");
		chord(_hardware, md::PanelControl::Kit);
		writePanelProbe(_hardware, "-1");
		pulse(_hardware, md::PanelControl::Enter);
		writePanelProbe(_hardware, "-2");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-3");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-4");
		pulse(_hardware, md::PanelControl::Right);
		writePanelProbe(_hardware, "-5");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-6");
		pulse(_hardware, md::PanelControl::Enter);
		writePanelProbe(_hardware, "-7");
		pulse(_hardware, md::PanelControl::Right);
		writePanelProbe(_hardware, "-8");
		pulse(_hardware, md::PanelControl::Enter);
		writePanelProbe(_hardware, "-9");
	}

	void enterMmDigiProReceive(md::Hardware& _hardware)
	{
		writePanelProbe(_hardware, "-0");
		chord(_hardware, md::PanelControl::Kit);
		writePanelProbe(_hardware, "-1");
		pulse(_hardware, md::PanelControl::Enter);
		writePanelProbe(_hardware, "-2");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-3");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-4");
		pulse(_hardware, md::PanelControl::Right);
		writePanelProbe(_hardware, "-5");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-6");
		pulse(_hardware, md::PanelControl::Down);
		writePanelProbe(_hardware, "-7");
		pulse(_hardware, md::PanelControl::Enter);
		writePanelProbe(_hardware, "-8");
		pulse(_hardware, md::PanelControl::Right);
		writePanelProbe(_hardware, "-9");
		pulse(_hardware, md::PanelControl::Enter);
		writePanelProbe(_hardware, "-10");
	}

	void writePanelProbe(const md::Hardware& _hardware, const char* const _suffix)
	{
		const auto* const prefix = std::getenv("MM_SYSEX_PANEL_PREFIX");
		if(prefix == nullptr || *prefix == '\0')
			return;
		const auto panel = _hardware.getFrontPanelSnapshot();
		std::ofstream output(std::string(prefix) + _suffix + ".pgm", std::ios::binary);
		output << "P5\n" << md::FrontPanel::g_lcdWidth << ' '
			<< md::FrontPanel::g_lcdHeight << "\n255\n";
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				output.put(panel.getLcdPixel(x, y) ? '\0' : '\xff');
	}

}

int main(const int _argc, char** _argv)
{
	if(_argc < 4 || _argc > 6)
	{
		std::fprintf(stderr,
			"usage:\n"
			"  mdUserSysexFirmwareTest md <rom> <user-data.syx> [first|cancel]\n"
			"  mdUserSysexFirmwareTest mm <rom> <1MiB-patch-ram> "
			"<user-data.syx> [first|cancel]\n");
		return 2;
	}

	const std::string modelName(_argv[1]);
	const bool monomachine = modelName == "mm";
	const int baseArgumentCount = monomachine ? 5 : 4;
	if((modelName != "md" && !monomachine)
		|| (_argc != baseArgumentCount && _argc != baseArgumentCount + 1)
		|| (_argc == baseArgumentCount + 1
			&& std::string(_argv[baseArgumentCount]) != "first"
			&& std::string(_argv[baseArgumentCount]) != "cancel"))
		return 2;
	const auto model = monomachine ? md::MachineModel::Monomachine
		: md::MachineModel::Machinedrum;
	const auto rom = load(_argv[2]);
	const auto patchRam = monomachine ? load(_argv[3]) : std::vector<uint8_t>{};
	const char* const sysexPath = _argv[monomachine ? 4 : 3];
	auto fileBytes = load(sysexPath);
	const bool firstMessageOnly = _argc == baseArgumentCount + 1
		&& std::string(_argv[baseArgumentCount]) == "first";
	const bool cancelMidMessage = _argc == baseArgumentCount + 1
		&& std::string(_argv[baseArgumentCount]) == "cancel";
	if(firstMessageOnly)
	{
		const auto end = std::find(fileBytes.begin(), fileBytes.end(), uint8_t{0xf7});
		if(end == fileBytes.end())
			return 2;
		fileBytes.erase(end + 1, fileBytes.end());
	}
	const auto validation = md::validateMidiSysexStream(fileBytes, model);
	if(validation != md::MidiSysexStreamValidation::Valid)
	{
		std::fprintf(stderr, "SysEx validation failed: %s\n",
			validationName(validation));
		return 2;
	}
	if(monomachine && patchRam.size() != 0x100000)
	{
		std::fputs("Monomachine patch RAM must be exactly 1 MiB\n", stderr);
		return 2;
	}

	md::Hardware hardware(rom, _argv[2], model, patchRam);
	if(!hardware.isValid())
	{
		std::fputs("firmware did not construct a valid machine\n", stderr);
		return 1;
	}
	const auto bootDeadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(180);
	while(!hardware.isFirmwareMidiReady() || (monomachine
		&& (!hardware.isAudioReady()
			|| hardware.getFrontPanelSnapshot().countLitPixels() <= 2000)))
	{
		hardware.advance(64);
		if(std::chrono::steady_clock::now() >= bootDeadline)
		{
			std::fprintf(stderr, "firmware MIDI boot timed out: pc=%08x\n",
				hardware.getUC().getPC());
			return 1;
		}
	}
	const bool digiPro = monomachine && fileBytes.size() > 6
		&& fileBytes[6] == 0x5d;
	if(monomachine)
	{
		if(digiPro)
			enterMmDigiProReceive(hardware);
		else
			enterMmGeneralSysexReceive(hardware);
	}

	const auto patchBefore = hardware.copyPatchRam();
	const auto flashBefore = hardware.copyFlashData();
	// Exercise all three ingress classes around the ownership boundary: a clock
	// already queued must cross before the file sender, while later semantic and
	// ordinary host events must remain intact behind it.
	const std::array<uint8_t, 1> clockBefore{uint8_t{synthLib::M_TIMINGCLOCK}};
	if(!hardware.trySendRealtimeMidi(clockBefore))
	{
		std::fputs("could not queue pre-transfer MIDI clock\n", stderr);
		return 1;
	}
	auto prepared = md::prepareMidiSysexTransfer(fileBytes);
	if(!prepared || !hardware.startMidiSysexTransfer(*prepared))
	{
		std::fputs("validated transfer did not start\n", stderr);
		return 1;
	}
	const std::array<uint8_t, 4> automationAfter{
		uint8_t{synthLib::M_CONTROLCHANGE}, uint8_t{synthLib::M_ALLNOTESOFF},
		0x00, uint8_t{synthLib::M_TIMINGCLOCK}};
	if(!hardware.trySendRealtimeMidi(automationAfter)
		|| !hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
			synthLib::M_NOTEOFF, 0x00, 0x00)))
	{
		std::fputs("could not queue concurrent MIDI arbitration traffic\n", stderr);
		return 1;
	}

	const auto transferStart = std::chrono::steady_clock::now();
	const auto transferDeadline = transferStart + std::chrono::seconds(180);
	uint8_t payloadSpeed = 1;
	bool cancellationRequested = false;
	for(;;)
	{
		hardware.advance(64);
		const auto progress = hardware.getMidiSysexTransferProgress();
		if(progress.state == md::MidiSysexTransferState::Sending)
		{
			payloadSpeed = progress.speedCode;
			if(cancelMidMessage && !cancellationRequested && progress.sent >= 32
				&& progress.sent < progress.total)
			{
				std::vector<uint8_t> retiredPayload;
				if(!hardware.cancelMidiSysexTransfer(retiredPayload)
					|| retiredPayload != fileBytes)
				{
					std::fputs("mid-message cancellation did not retire its payload\n", stderr);
					return 1;
				}
				cancellationRequested = true;
			}
		}
		if((!cancelMidMessage
				&& progress.state == md::MidiSysexTransferState::Complete)
			|| (cancelMidMessage && cancellationRequested
				&& progress.state == md::MidiSysexTransferState::Cancelled))
			break;
		if(std::chrono::steady_clock::now() >= transferDeadline)
		{
			std::fprintf(stderr, "transfer timed out: sent=%zu/%zu state=%u\n",
				progress.sent, progress.total,
				static_cast<unsigned>(progress.state));
			return 1;
		}
	}
	if(cancelMidMessage)
	{
		while(!hardware.isMidiIngressIdle()
			&& std::chrono::steady_clock::now() < transferDeadline)
			hardware.advance(64);
		const auto finalProgress = hardware.getMidiSysexTransferProgress();
		std::printf("[%s SysEx firmware cancellation test] sent=%zu/%zu "
			"payload=%sx state=%u idle=%u\n", monomachine ? "MM" : "MD",
			finalProgress.sent, finalProgress.total,
			md::midiTurboSpeedLabel(payloadSpeed),
			static_cast<unsigned>(finalProgress.state), hardware.isMidiIngressIdle());
		return cancellationRequested && payloadSpeed > 1
			&& finalProgress.state == md::MidiSysexTransferState::Cancelled
			&& hardware.isMidiIngressIdle() ? 0 : 1;
	}

	// UART-drained is a transport result. Give firmware time to validate, unpack,
	// and commit the last message, then require an externally observable mutation.
	for(size_t i = 0; i < 1200; ++i)
		hardware.advance(64);
	if(monomachine)
	{
		writePanelProbe(hardware, "-after");
		// Match the hardware workflow: leave the receive screen so firmware can
		// finish any deferred store operation before persistence is inspected.
		pulse(hardware, md::PanelControl::Exit);
		if(digiPro)
		{
			const auto storeDeadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(60);
			size_t stableObservations = 0;
			auto observed = flashBefore;
			do
			{
				settle(hardware, 200);
				auto current = hardware.copyFlashData();
				if(current != observed)
				{
					observed = std::move(current);
					stableObservations = 0;
				}
				else if(observed != flashBefore)
					++stableObservations;
			} while(stableObservations < 10
				&& std::chrono::steady_clock::now() < storeDeadline);
		}
		else
			settle(hardware, 1000);
		writePanelProbe(hardware, "-exit");
	}
	if(!hardware.isMidiIngressIdle())
	{
		std::fputs("MIDI queued around the transfer did not drain afterward\n", stderr);
		return 1;
	}
	const auto patchAfter = hardware.copyPatchRam();
	const auto flashAfter = hardware.copyFlashData();
	const auto changedPatch = changedBytes(patchBefore, patchAfter);
	const auto changedFlash = changedBytes(flashBefore, flashAfter);
	size_t stateBytes = 0;
	if(digiPro)
	{
		const auto userFlash = hardware.copyUserFlash();
		std::vector<uint8_t> state;
		md::DecodedState decoded;
		if(userFlash.size() != md::g_mmUserFlashStateSize
			|| !md::encodeState(state, patchAfter, model,
				synthLib::StateTypeGlobal, userFlash)
			|| !md::decodeState(decoded, state, {}, model,
				synthLib::StateTypeGlobal))
		{
			std::fputs("DigiPRO project-state persistence failed\n", stderr);
			return 1;
		}
		md::Hardware restored(rom, _argv[2], model, decoded.patchRam,
			std::shared_ptr<md::FrontPanelPublisher>{}, std::vector<uint8_t>{},
			std::vector<uint8_t>{}, md::FlashSectorOverlay{}, decoded.userFlash);
		if(!restored.isValid() || restored.copyUserFlash() != userFlash)
		{
			std::fputs("DigiPRO flash did not survive a machine-state rebuild\n", stderr);
			return 1;
		}
		stateBytes = state.size();
	}
	const auto elapsed = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - transferStart).count();
	std::printf("[%s SysEx firmware test] bytes=%zu payload=%sx "
		"patchChanged=%zu flashChanged=%zu stateBytes=%zu wall=%.3fs\n",
		monomachine ? "MM" : "MD", fileBytes.size(),
		md::midiTurboSpeedLabel(payloadSpeed), changedPatch, changedFlash,
		stateBytes, elapsed);
	return payloadSpeed > 1 && (changedPatch != 0 || changedFlash != 0) ? 0 : 1;
}
