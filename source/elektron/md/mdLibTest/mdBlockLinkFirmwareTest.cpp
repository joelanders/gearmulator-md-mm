#include "mdLib/mdhardware.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The block link (mdLib/mdblocklink.h) against the serial path it replaces, on Machinedrum OS 1.63: it has to
// engage, and a triggered sound has to come out as the same waveform, moved by whole 32 sample blocks at most.

namespace
{
	constexpr uint32_t g_hitFrames = 12288;
	constexpr uint32_t g_holdFrames = 2048;
	constexpr int g_hits = 6;
	constexpr int g_blockFrames = 32;
	constexpr int g_maxBlocksApart = 8;

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

	struct Render
	{
		bool blockLink = false;
		std::vector<std::vector<double>> hits;		// mono, one vector per trigger
	};

	void advance(md::Hardware& _hardware, uint32_t _frames)
	{
		while(_frames)
		{
			const auto count = std::min<uint32_t>(_frames, 64);
			_hardware.advance(count);
			_frames -= count;
		}
	}

	Render render(const std::vector<uint8_t>& _rom, const char* const _path, const bool _blockLink)
	{
		// The switch is read when the machine is built
		setEnvironment("GEARMULATOR_MDMM_BLOCK_LINK", _blockLink ? nullptr : "0");
		md::Hardware hardware(_rom, _path, md::MachineModel::Machinedrum);
		setEnvironment("GEARMULATOR_MDMM_BLOCK_LINK", nullptr);
		require(hardware.isValid(), "machine was invalid");

		uint32_t waited = 0;
		while(!hardware.isFirmwareMidiReady())
		{
			advance(hardware, 64);
			waited += 64;
			require(waited < md::g_samplerate * 30, "firmware MIDI did not become ready");
		}
		// A machine without stored flash runs its first-boot initialization, which takes the firmware about 15 s
		advance(hardware, md::g_samplerate * 20);

		// GND-IMP on track 1: one impulse per trigger, the same every time
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x5b, 0x00, 0x03, 0x02, 0xf7};
		hardware.sendMidi(assign);
		advance(hardware, md::g_samplerate * 5 / 2);

		const auto trigger = md::panelPacket(md::MachineModel::Machinedrum, md::PanelControl::Trigger1);
		require(trigger.has_value(), "no panel packet for Trigger1");

		Render result;
		result.blockLink = hardware.isBlockLinkActive();

		std::array<std::vector<float>, 6> channels;
		for(auto& channel : channels)
			channel.resize(g_hitFrames + 64);
		synthLib::TAudioOutputs outputs{};

		/*	The first trigger after the assignment is played and not compared. GND-IMP answers it with an extra low
			thump on both paths, but the serial path leaves it out when one of its occasional short host messages
			to the producer falls just before the trigger (at 1.5 s after the assignment it does, from 1.6 s to 8 s
			it does not), so that trigger says nothing about the link.
		*/
		for(int hit = -1; hit < g_hits; ++hit)
		{
			// 37 has no factor in common with 32: the triggers fall on different positions of the firmware block
			const uint32_t frames = g_hitFrames + static_cast<uint32_t>(((hit + 1) * 37) % 64);
			for(size_t c = 0; c < channels.size(); ++c)
				outputs[c] = channels[c].data();
			hardware.sendPanelEvent(trigger->row, trigger->mask);
			hardware.processAudio(outputs, g_holdFrames, 0);
			hardware.sendPanelEvent(trigger->row, 0);
			for(size_t c = 0; c < channels.size(); ++c)
				outputs[c] = channels[c].data() + g_holdFrames;
			hardware.processAudio(outputs, frames - g_holdFrames, 0);

			if(hit < 0)
				continue;

			std::vector<double> mono(g_hitFrames);
			for(uint32_t f = 0; f < g_hitFrames; ++f)
			{
				mono[f] = 0.5 * (static_cast<double>(channels[0][f]) + static_cast<double>(channels[1][f]));
				require(std::isfinite(mono[f]), "non-finite audio");
			}
			result.hits.push_back(std::move(mono));
		}
		return result;
	}

	double energy(const std::vector<double>& _audio)
	{
		double sum = 0.0;
		for(const auto v : _audio)
			sum += v * v;
		return sum;
	}

	// Energy of (_a - _b moved by _lag frames) over the part both cover, relative to _a
	double residual(const std::vector<double>& _a, const std::vector<double>& _b, const int _lag)
	{
		const int margin = g_blockFrames * g_maxBlocksApart;
		const int count = static_cast<int>(_a.size()) - 2 * margin;
		double difference = 0.0;
		double reference = 0.0;
		for(int i = margin; i < margin + count; ++i)
		{
			const double d = _a[i] - _b[i + _lag];
			difference += d * d;
			reference += _a[i] * _a[i];
		}
		return reference > 0.0 ? difference / reference : 1.0;
	}
}

int main()
{
	const auto* const path = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	if(path == nullptr || *path == '\0')
	{
		std::cout << "Set GEARMULATOR_MD_FIRMWARE_BIN to Machinedrum OS 1.63\n";
		return 77;
	}

	try
	{
		std::ifstream file(path, std::ios::binary);
		const std::vector<uint8_t> rom{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum),
			"firmware is not the canonical Machinedrum OS 1.63 image");

		const auto serial = render(rom, path, false);
		const auto block = render(rom, path, true);

		require(!serial.blockLink, "GEARMULATOR_MDMM_BLOCK_LINK=0 did not keep the serial path");
		require(block.blockLink, "the block link did not engage on Machinedrum OS 1.63");

		for(int hit = 0; hit < g_hits; ++hit)
		{
			const auto& a = serial.hits[hit];
			const auto& b = block.hits[hit];
			const auto label = "trigger " + std::to_string(hit) + ": ";

			const double energyA = energy(a);
			const double energyB = energy(b);
			require(energyA > 1.0e-3, label + "the serial path is silent");
			require(std::fabs(energyB - energyA) <= 1.0e-3 * energyA, label + "the block link changed the level");

			// The same waveform, at most a few firmware blocks apart and never a fraction of a block
			double best = 1.0;
			int bestLag = 0;
			for(int blocks = -g_maxBlocksApart; blocks <= g_maxBlocksApart; ++blocks)
			{
				const double r = residual(a, b, blocks * g_blockFrames);
				if(r < best)
				{
					best = r;
					bestLag = blocks * g_blockFrames;
				}
			}
			// -50 dB. A sound that differs lands near 0 dB, the matches measured -69 to -96 dB.
			require(best < 1.0e-5, label + "the block link output is not the serial waveform moved by whole blocks");
			std::cout << label << "same waveform " << bestLag << " frames later, residual "
				<< 10.0 * std::log10(std::max(best, 1.0e-30)) << " dB\n";
		}

		std::cout << "mdBlockLinkFirmwareTest: PASS\n";
		return 0;
	}
	catch(const std::exception& e)
	{
		std::cerr << "mdBlockLinkFirmwareTest: " << e.what() << '\n';
		return 1;
	}
}
