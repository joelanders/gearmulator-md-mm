#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
	void require(bool condition, const char* message)
	{
		if(!condition)
			throw std::runtime_error(message);
	}

	void advance(md::Hardware& hardware, uint32_t frames)
	{
		while(frames)
		{
			const auto chunk = std::min<uint32_t>(256, frames);
			hardware.advance(chunk);
			frames -= chunk;
		}
	}

	double render(md::Hardware& hardware)
	{
		std::array<std::array<float, 256>, 2> samples{};
		synthLib::TAudioOutputs outputs{};
		outputs[0] = samples[0].data();
		outputs[1] = samples[1].data();
		double sum = 0;
		for(unsigned block = 0; block < 64; ++block)
		{
			hardware.processAudio(outputs, 256, 0);
			for(const auto& channel : samples)
				for(const auto sample : channel)
				{
					require(std::isfinite(sample), "non-finite MM audio");
					sum += double(sample) * sample;
				}
		}
		return std::sqrt(sum / (64 * 256 * 2));
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path || !*path)
	{
		std::cout << "mmAudioFirmwareTest: SKIP (MM firmware not supplied)\n";
		return 77;
	}
	try
	{
		std::vector<uint8_t> rom;
		require(baseLib::filesystem::readFile(rom, path), "could not read MM fixture");
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Monomachine),
			"MM fixture fingerprint mismatch");
		auto machine = std::make_unique<md::Hardware>(rom, path, md::MachineModel::Monomachine);
		auto& hardware = *machine;
		advance(hardware, md::g_samplerate * 20);
		require(hardware.isAudioReady() && hardware.isFirmwareMidiReady(), "MM boot incomplete");
		// Fresh hardware starts without patch RAM supplied by the host. Exercise
		// its firmware-initialized kit through ordinary MIDI, not private memory.
		require(render(hardware) < 1e-7, "idle MM unexpectedly produced audio");
		for(uint8_t track = 0; track < 6; ++track)
		{
			// CC7 is the documented per-track level control. Verify that the DSP
			// actually reacts to parameter traffic, not just that MIDI was accepted.
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0xb0 | track), 7, 0)), "level change rejected");
			advance(hardware, md::g_samplerate * 2);
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0x90 | track), 60, 100)), "quiet note-on rejected");
			const auto quiet = render(hardware);
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0x80 | track), 60, 0)), "quiet note-off rejected");
			advance(hardware, md::g_samplerate * 2);
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0xb0 | track), 7, 127)), "level restore rejected");
			advance(hardware, md::g_samplerate / 10);
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0x90 | track), 60, 100)), "note-on rejected");
			const auto rms = render(hardware);
			std::cout << "track " << unsigned(track) << " RMS " << rms
				<< ", zero-level RMS " << quiet << '\n';
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0x80 | track), 60, 0)), "note-off rejected");
			advance(hardware, md::g_samplerate);
			require(rms > 1e-5, "MM note produced silence");
			require(quiet < rms * 0.1, "MM level control did not attenuate audio");
		}
		std::cout << "mmAudioFirmwareTest: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mmAudioFirmwareTest: " << error.what() << '\n';
		return 1;
	}
}
