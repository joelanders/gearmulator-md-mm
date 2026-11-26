#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// The Monomachine block link (mdLib/mmblocklink.h) against the serial path it replaces, on OS 1.32b: it has
// to stay off unless asked for, engage when asked for, and a sine played on a track of either DSP has to
// come out as loud and as smooth as on the serial path. The waveform of a Monomachine note depends on the
// sample it starts on, so the two paths cannot be compared sample by sample; a missing or stale word of the
// link, or a missing output frame, shows as a step in the waveform, which the smoothness measure sees.

namespace
{
	constexpr uint32_t g_blocks = 64;				// of 256 frames per note
	constexpr uint32_t g_blockFrames = 256;

	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void setEnvironment(const char* const _name, const char* const _value)
	{
#ifdef _WIN32
		_putenv_s(_name, _value ? _value : "");
#else
		if(_value)
			setenv(_name, _value, 1);
		else
			unsetenv(_name);
#endif
	}

	void advance(md::Hardware& _hardware, uint32_t _frames)
	{
		while(_frames)
		{
			const auto count = std::min<uint32_t>(_frames, 64);
			_hardware.advance(count);
			_frames -= count;
		}
	}

	struct Note
	{
		double rms = 0.0;
		double roughness = 0.0;		// energy of the second difference over the energy, second half of the note
	};

	struct Render
	{
		bool blockLink = false;
		std::vector<Note> notes;	// per track and note, in the order played
	};

	constexpr std::array<uint8_t, 2> g_tracks = {0, 5};			// track 1 is DSP2's and crosses the link, track 6 is DSP1's
	constexpr std::array<uint8_t, 3> g_notes = {36, 48, 72};

	Note play(md::Hardware& _hardware, const uint8_t _track, const uint8_t _note)
	{
		std::array<std::array<float, g_blockFrames>, 2> samples{};
		synthLib::TAudioOutputs outputs{};
		outputs[0] = samples[0].data();
		outputs[1] = samples[1].data();

		require(_hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
			static_cast<uint8_t>(0x90 | _track), _note, 100)), "note-on rejected");

		double sum = 0.0, power = 0.0, difference = 0.0;
		std::array<double, 2> previous{}, beforePrevious{};
		for(uint32_t block = 0; block < g_blocks; ++block)
		{
			_hardware.processAudio(outputs, g_blockFrames, 0);
			for(size_t channel = 0; channel < samples.size(); ++channel)
			{
				for(const auto sample : samples[channel])
				{
					require(std::isfinite(sample), "non-finite audio");
					sum += static_cast<double>(sample) * sample;
					if(block >= g_blocks / 2)
					{
						const auto delta = sample - 2.0 * previous[channel] + beforePrevious[channel];
						difference += delta * delta;
						power += static_cast<double>(sample) * sample;
					}
					beforePrevious[channel] = previous[channel];
					previous[channel] = sample;
				}
			}
		}

		require(_hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
			static_cast<uint8_t>(0x80 | _track), _note, 0)), "note-off rejected");
		advance(_hardware, md::g_samplerate / 2);

		Note note;
		note.rms = std::sqrt(sum / (g_blocks * g_blockFrames * 2));
		note.roughness = power > 0.0 ? difference / power : 0.0;
		return note;
	}

	Render render(const std::vector<uint8_t>& _rom, const char* const _path, const bool _blockLink)
	{
		// The switch is read when the machine is built
		setEnvironment("GEARMULATOR_MM_BLOCK_LINK", _blockLink ? "1" : nullptr);
		md::Hardware hardware(_rom, _path, md::MachineModel::Monomachine);
		setEnvironment("GEARMULATOR_MM_BLOCK_LINK", nullptr);
		require(hardware.isValid() && hardware.isMonomachine(), "machine was invalid");

		advance(hardware, md::g_samplerate * 20);
		require(hardware.isAudioReady() && hardware.isFirmwareMidiReady(), "the Monomachine did not boot");

		Render result;
		result.blockLink = hardware.isBlockLinkActive();

		// Manufacturer manual, Appendix C: machine 01 is GND-SIN, init=1 initializes all data pages
		for(const auto track : g_tracks)
		{
			synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
			assign.sysex = {0xf0, 0, 0x20, 0x3c, 3, 0, 0x5b, track, 1, 1, 0xf7};
			require(hardware.sendMidi(assign), "GND-SIN assignment rejected");
			advance(hardware, md::g_samplerate);
		}

		for(const auto track : g_tracks)
			for(const auto note : g_notes)
				result.notes.push_back(play(hardware, track, note));
		return result;
	}
}

int main()
{
	const auto* const path = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(path == nullptr || *path == '\0')
	{
		std::cout << "Set GEARMULATOR_MM_FIRMWARE_BIN to Monomachine OS 1.32b\n";
		return 77;
	}

	try
	{
		std::ifstream file(path, std::ios::binary);
		const std::vector<uint8_t> rom{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Monomachine),
			"firmware is not the canonical Monomachine OS 1.32b image");

		const auto serial = render(rom, path, false);
		const auto block = render(rom, path, true);

		require(!serial.blockLink, "the block link engaged without GEARMULATOR_MM_BLOCK_LINK=1");
		require(block.blockLink, "the block link did not engage on Monomachine OS 1.32b");

		size_t index = 0;
		for(const auto track : g_tracks)
		{
			for(const auto note : g_notes)
			{
				const auto& a = serial.notes[index];
				const auto& b = block.notes[index];
				++index;
				const auto label = "track " + std::to_string(track + 1) + " note " + std::to_string(note) + ": ";

				// A step in the waveform, one sample that is zero or a block old, adds to the energy of the second
				// difference: about a thousand times what a sine of note 48 has. Both paths play the same
				// sound, so their smoothness may differ by the phase the note happens to start in, not by more.
				require(a.rms > 1.0e-3, label + "the serial path is silent");
				require(a.roughness > 0.0, label + "the serial path has no waveform");
				require(std::fabs(b.rms - a.rms) <= 0.05 * a.rms, label + "the block link changed the level");
				require(b.roughness < a.roughness * 4.0 && b.roughness > a.roughness * 0.25,
					label + "the block link output is not as smooth as the serial path's");
				std::cout << label << "RMS " << a.rms << " / " << b.rms << ", roughness " << a.roughness << " / "
					<< b.roughness << " (serial / block link)\n";
			}
		}

		std::cout << "mmBlockLinkFirmwareTest: PASS\n";
		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "mmBlockLinkFirmwareTest: " << e.what() << '\n';
		return 1;
	}
}
