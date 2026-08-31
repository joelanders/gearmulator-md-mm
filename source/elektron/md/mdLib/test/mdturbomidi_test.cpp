#include "mdLib/mdhardware.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace
{
	std::vector<uint8_t> load(const char* const _path)
	{
		std::ifstream input(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	}
}

int main(int _argc, char** _argv)
{
	if(_argc != 3)
	{
		std::fprintf(stderr,
			"usage: mdturbomidi_test <machinedrum-rom> <machinedrum-sysex-file>\n");
		return 2;
	}

	const auto rom = load(_argv[1]);
	auto bytes = load(_argv[2]);
	if(bytes.empty())
	{
		std::fprintf(stderr, "cannot read SysEx file: %s\n", _argv[2]);
		return 2;
	}

	md::Hardware hardware(rom, _argv[1], md::MachineModel::Machinedrum);
	if(!hardware.isValid())
	{
		std::printf("[TurboMIDI test] SKIP: Machinedrum firmware not found\n");
		return 77;
	}

	const auto bootStart = std::chrono::steady_clock::now();
	while(hardware.getFrontPanelSnapshot().countLitPixels() <= 2000)
	{
		hardware.advance(64);
		if(std::chrono::steady_clock::now() - bootStart > std::chrono::seconds(90))
		{
			std::fprintf(stderr, "[TurboMIDI test] firmware boot timed out\n");
			return 1;
		}
	}

	auto prepared = md::prepareMidiSysexTransfer(std::move(bytes));
	if(!prepared || !hardware.startMidiSysexTransfer(*prepared))
	{
		std::fprintf(stderr, "[TurboMIDI test] transfer did not start\n");
		return 1;
	}

	const auto transferStart = std::chrono::steady_clock::now();
	uint8_t maximumSpeed = 1;
	uint8_t payloadSpeed = 1;
	for(;;)
	{
		hardware.advance(64);
		const auto progress = hardware.getMidiSysexTransferProgress();
		maximumSpeed = std::max(maximumSpeed, progress.speedCode);
		if(progress.state == md::MidiSysexTransferState::Sending)
			payloadSpeed = progress.speedCode;
		if(progress.state == md::MidiSysexTransferState::Complete)
		{
			const auto elapsed = std::chrono::duration<double>(
				std::chrono::steady_clock::now() - transferStart).count();
			std::printf("[TurboMIDI test] complete: bytes=%zu maxObserved=%sx payload=%sx wall=%.3fs\n",
				progress.total, md::midiTurboSpeedLabel(maximumSpeed),
				md::midiTurboSpeedLabel(payloadSpeed), elapsed);
			// speed1's link test can complete between two public progress polls;
			// payloadSpeed is stable for the whole file and proves Turbo was engaged.
			return payloadSpeed > 1 ? 0 : 1;
		}
		if(std::chrono::steady_clock::now() - transferStart > std::chrono::seconds(90))
		{
			std::fprintf(stderr, "[TurboMIDI test] transfer timed out (maxObserved=%sx, payload=%sx)\n",
				md::midiTurboSpeedLabel(maximumSpeed),
				md::midiTurboSpeedLabel(payloadSpeed));
			return 1;
		}
	}
}
