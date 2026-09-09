#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
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

	struct DifferenceEnergy
	{
		double previous = 0, beforePrevious = 0, power = 0, difference = 0;
		void add(double sample, bool measure)
		{
			if(measure)
			{
				const auto delta = sample - 2 * previous + beforePrevious;
				difference += delta * delta;
				power += sample * sample;
			}
			beforePrevious = previous;
			previous = sample;
		}
	};

	double render(md::Hardware& hardware, double* roughness = nullptr, unsigned blocks = 64,
		bool reportIdle = false)
	{
		std::array<std::array<float, 256>, 2> samples{};
		synthLib::TAudioOutputs outputs{};
		outputs[0] = samples[0].data();
		outputs[1] = samples[1].data();
		double sum = 0;
		std::array<DifferenceEnergy, 2> energy{};
		std::array<double, 2> windowSum{}, windowPower{}, windowPeak{};
		unsigned windowFrames = 0;
		for(unsigned block = 0; block < blocks; ++block)
		{
			hardware.processAudio(outputs, 256, 0);
			for(size_t channel = 0; channel < samples.size(); ++channel)
				for(const auto sample : samples[channel])
				{
					require(std::isfinite(sample), "non-finite MM audio");
					sum += double(sample) * sample;
					energy[channel].add(sample, block >= blocks / 2);
					if(reportIdle)
					{
						windowSum[channel] += sample;
						windowPower[channel] += double(sample) * sample;
						windowPeak[channel] = std::max(windowPeak[channel], std::abs(double(sample)));
					}
				}
			windowFrames += 256;
			if(reportIdle && ((block + 1) % 16 == 0 || block + 1 == blocks))
			{
				for(size_t channel = 0; channel < samples.size(); ++channel)
				{
					const auto mean = windowSum[channel] / windowFrames;
					const auto power = windowPower[channel] / windowFrames;
					std::cout << "Idle window ending frame " << (block + 1) * 256
						<< " channel " << channel << " mean " << mean
						<< " rms " << std::sqrt(power)
						<< " ac-rms " << std::sqrt(std::max(0.0, power - mean * mean))
						<< " peak " << windowPeak[channel] << '\n';
				}
				windowSum.fill(0);
				windowPower.fill(0);
				windowPeak.fill(0);
				windowFrames = 0;
			}
		}
		if(roughness)
		{
			const auto power = energy[0].power + energy[1].power;
			*roughness = power > 0 ? (energy[0].difference + energy[1].difference) / power : 0;
		}
		return std::sqrt(sum / (blocks * 256 * 2));
	}

	void tap(md::Hardware& hardware, md::PanelControl control)
	{
		const auto packet = md::panelPacket(md::MachineModel::Monomachine, control);
		require(packet.has_value(), "missing MM panel control");
		require(hardware.trySendPanelEvent(packet->row, packet->mask), "panel press rejected");
		advance(hardware, 2048);
		require(hardware.trySendPanelEvent(packet->row, 0), "panel release rejected");
		advance(hardware, 4096);
	}

	void requireSmoothSine(double rms, double roughness, uint8_t note)
	{
		// For x[n] = sin(w*n), normalized second-difference energy is
		// (2 - 2*cos(w))^2. Allow envelope/filter settling, but reject silence,
		// DC and large inter-sample discontinuities. This is not a full spectral oracle.
		const auto frequency = 440.0 * std::exp2((double(note) - 69.0) / 12.0);
		const auto difference = 2.0 - 2.0 * std::cos(6.283185307179586 * frequency / md::g_samplerate);
		const auto expected = difference * difference;
		require(std::isfinite(rms) && rms > 1e-5, "GND SIN produced silence/non-finite audio");
		require(std::isfinite(roughness) && roughness > expected * 0.25
			&& roughness < expected * 4, "GND SIN waveform failed smoothness/pitch-scale check");
	}

	void testSineOracle()
	{
		for(unsigned mode = 0; mode < 6; ++mode)
		{
			DifferenceEnergy energy;
			for(unsigned frame = 0; frame < 8192; ++frame)
			{
				double sample = std::sin(6.283185307179586 * 261.6255653006 * frame / md::g_samplerate);
				if(mode == 1) sample = 0;
				if(mode == 2) sample = 0.5;
				if(mode == 3 && (frame & 15) == 0) sample = 0;
				if(mode == 4) sample = std::numeric_limits<double>::quiet_NaN();
				if(mode == 5) sample = std::numeric_limits<double>::infinity();
				energy.add(sample, frame >= 4096);
			}
			bool accepted = true;
			try
			{
				requireSmoothSine(std::sqrt(energy.power / 4096),
					energy.power > 0 ? energy.difference / energy.power : 0, 60);
			}
			catch(const std::runtime_error&) { accepted = false; }
			require(accepted == (mode == 0), "sine oracle positive/negative control failed");
		}
	}

	void loadEmptyKit(md::Hardware& hardware)
	{
		// Manual: KIT > LOAD, FUNCTION+PLAY clears the selected kit, ENTER loads
		// it. An empty kit initializes all six tracks to GND>SIN. Only this fresh
		// in-memory test machine is changed; no user project is loaded or saved.
		tap(hardware, md::PanelControl::Kit);
		tap(hardware, md::PanelControl::Enter);
		const auto function = md::panelPacket(md::MachineModel::Monomachine, md::PanelControl::Function);
		const auto play = md::panelPacket(md::MachineModel::Monomachine, md::PanelControl::Play);
		require(function && play, "missing clear-kit controls");
		md::PanelRowState rows;
		const std::array packets{rows.press(*function), rows.press(*play),
			rows.release(*play), rows.release(*function)};
		for(const auto packet : packets)
		{
			require(hardware.trySendPanelEvent(packet.row, packet.mask), "clear-kit control rejected");
			advance(hardware, 2048);
		}
		advance(hardware, md::g_samplerate * 2);
		tap(hardware, md::PanelControl::Enter);
		tap(hardware, md::PanelControl::Exit);
	}

	void testAudioInput(md::Hardware& hardware)
	{
		const auto settle = [&](uint32_t frames) {
			while(frames) { const auto n=std::min(256u,frames); hardware.processAudio(n,0); frames-=n; }
		};
		const auto sysex = [&](std::initializer_list<uint8_t> bytes) {
			synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
			event.sysex.assign(bytes.begin(),bytes.end());
			require(hardware.sendMidi(event), "input setup SysEx rejected");
		};
		std::array<std::array<float,256>,2> input{}, output{};
		synthLib::TAudioInputs ins{}; synthLib::TAudioOutputs outs{};
		for(unsigned c=0;c<2;++c) { ins[c]=input[c].data(); outs[c]=output[c].data(); }
		for(uint8_t track=0;track<6;++track)
		{
			// Manufacturer Appendix C: FX THRU=12, output AB mask=1,
			// input A+B=3. Configure through MIDI; no private DSP RAM edits.
			sysex({0xf0,0,0x20,0x3c,3,0,0x5b,track,12,1,0xf7}); settle(44100);
			sysex({0xf0,0,0x20,0x3c,3,0,0x5c,track,1,3,0xf7});
			for(auto cc : {std::pair<uint8_t,uint8_t>{55,127},{56,0},{57,0},{58,127},
				{59,127},{7,100},{84,64},{85,0}})
				require(hardware.sendMidi({synthLib::MidiEventSource::Host,
					static_cast<uint8_t>(0xb0|track),cc.first,cc.second}), "input setup CC rejected");
			settle(44100);
			require(hardware.sendMidi({synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0x90|track),60,100}), "THRU gate rejected");
			settle(4096);
			hardware.processAudio(outs,256,0);
			for(const auto& channel:output) for(float sample:channel)
				require(std::abs(sample)<1e-5f, "THRU emitted audio with silent input");
			hardware.resetHostAudioInputQueueTelemetry();
			double power=0, sine=0, cosine=0;
			constexpr unsigned total=8192, discard=2048, measured=total-discard;
			const double omega=6.283185307179586*(310+37*track)/44100;
			for(unsigned at=0;at<total;at+=256)
			{
				for(unsigned i=0;i<256;++i) for(unsigned c=0;c<2;++c)
					input[c][i]=static_cast<float>(.05*std::sin(omega*(at+i)));
				hardware.processAudio(ins,outs,256,0);
				for(unsigned i=0;i<256;++i) if(at+i>=discard) {
					const auto sample=output[0][i]; require(std::isfinite(sample), "non-finite input output");
					power+=sample*sample; sine+=sample*std::sin(omega*(at+i)); cosine+=sample*std::cos(omega*(at+i));
				}
			}
			const auto rms=std::sqrt(power/measured);
			const auto fraction=power>0 ? 2*(sine*sine+cosine*cosine)/(measured*power) : 0;
			std::cout<<"MM input track "<<unsigned(track+1)<<" RMS="<<rms<<" fundamental fraction="<<fraction<<'\n';
			require(rms>1e-4 && fraction>.8, "THRU input missing or failed frequency oracle");
			require(hardware.hostAudioInputUnderflowCount()==0 && hardware.hostAudioInputOverflowCount()==0,
				"host input queue lost continuity");
			hardware.sendMidi({synthLib::MidiEventSource::Host,static_cast<uint8_t>(0xb0|track),7,0}); settle(44100);
		}
	}
}

int main(int argc, char** argv)
{
	if(argc == 2 && std::string_view(argv[1]) == "--sine-oracle")
	{
		try { testSineOracle(); return 0; }
		catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
	}
	const bool sineMidi = argc == 2 && std::string_view(argv[1]) == "--sine-midi";
	const bool input = argc == 2 && std::string_view(argv[1]) == "--input";
	const bool sine = sineMidi || (argc == 2 && std::string_view(argv[1]) == "--sine");
	const bool ensemble = argc == 2 && std::string_view(argv[1]) == "--digipro-ensemble";
	const bool digipro = ensemble || (argc == 2 && std::string_view(argv[1]) == "--digipro");
	if(argc != 1 && !sine && !digipro && !input)
		return 2;
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
		if(sine || digipro || input)
			loadEmptyKit(hardware);
		if(input) { testAudioInput(hardware); return 0; }
		if(sineMidi)
		{
			// Manufacturer manual, Appendix C: machine 01 is GND-SIN, and
			// init=1 initializes all data pages. Preserve the original panel-only
			// sine fixture as a separate gate rather than replacing its coverage.
			for(uint8_t track = 0; track < 6; ++track)
			{
				synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
				assign.sysex = {0xf0, 0, 0x20, 0x3c, 3, 0, 0x5b, track, 1, 1, 0xf7};
				require(hardware.sendMidi(assign), "GND SIN assignment rejected");
				advance(hardware, md::g_samplerate);
			}
		}
		// Fresh hardware starts without patch RAM supplied by the host. Exercise
		// its firmware-initialized kit through ordinary MIDI, not private memory.
		// Observe the same samples used by the strict gate, without extra
		// settling frames or removing DC from its pass/fail measurement.
		const auto idleRms = render(hardware, nullptr, 64, true);
		std::cout << "Idle MM RMS " << idleRms << '\n';
		require(idleRms < 1e-7, "idle MM unexpectedly produced audio");
		for(uint8_t track = 0; track < 6; ++track)
		{
			if(digipro)
			{
				// Public manual, Appendix C: assign DPRO-DDRW (32) or DPRO-DENS (33)
				// and initialize its data pages. No host writes to private kit RAM.
				synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
				assign.sysex = {0xf0, 0, 0x20, 0x3c, 3, 0, 0x5b, track,
					static_cast<uint8_t>(ensemble ? 33 : 32), 1, 0xf7};
				require(hardware.sendMidi(assign), "DigiPRO assignment rejected");
				advance(hardware, md::g_samplerate);
			}
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
			double roughness = 0;
			const auto rms = render(hardware, &roughness);
			std::cout << "track " << unsigned(track) << " RMS " << rms
				<< ", zero-level RMS " << quiet << ", roughness " << roughness << '\n';
			require(rms > 1e-5, "MM note produced silence");
			require(quiet < rms * 0.1, "MM level control did not attenuate audio");
			if(digipro)
			{
				// Appendix A: WAV1/WAV2 select the 64-wave bank. Appendix B:
				// synthesis parameters 1/3 are CC48/50. Sweep the entire MIDI
				// range without assuming how its 128 values map onto 64 slots.
				// DPRO-DENS instead has WAVE as synthesis parameter 4 (CC51).
				const std::vector<uint8_t> waveControllers = ensemble
					? std::vector<uint8_t>{51} : std::vector<uint8_t>{48, 50};
				for(const uint8_t cc : waveControllers)
				{
					// Sweep each slot independently: changing both together lets one
					// working selector hide a broken one. DDRW's MIX (parameter 2,
					// CC49) makes only the selected slot audible at either endpoint.
					if(!ensemble)
						require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
							static_cast<uint8_t>(0xb0 | track), 49, cc == 48 ? 0 : 127)),
							"DigiPRO waveform mix change rejected");
					double minRoughness = std::numeric_limits<double>::infinity();
					double maxRoughness = 0;
					for(unsigned value = 0; value < 128; ++value)
					{
						require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
							static_cast<uint8_t>(0x80 | track), 60, 0)), "DigiPRO sweep note-off rejected");
						require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
							static_cast<uint8_t>(0xb0 | track), cc, static_cast<uint8_t>(value))),
							"DigiPRO waveform change rejected");
						advance(hardware, md::g_samplerate / 10);
						// Retrigger each observation: the default amplitude envelope
						// decays even while a MIDI key remains held.
						require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
							static_cast<uint8_t>(0x90 | track), 60, 100)), "DigiPRO sweep note-on rejected");
						double waveRoughness = 0;
						const auto waveRms = render(hardware, &waveRoughness, 16);
						std::cout << "DigiPRO track " << unsigned(track) << " CC " << unsigned(cc)
							<< " value " << value << " RMS " << waveRms
							<< " roughness " << waveRoughness << '\n';
						require(waveRms > 1e-5, "DigiPRO waveform produced silence");
						minRoughness = std::min(minRoughness, waveRoughness);
						maxRoughness = std::max(maxRoughness, waveRoughness);
					}
					std::cout << "DigiPRO track " << unsigned(track) << " CC " << unsigned(cc)
						<< " roughness range " << minRoughness << ".." << maxRoughness << '\n';
					require(maxRoughness > minRoughness * 2, "DigiPRO waveform sweep did not change timbre");
				}
			}
			if(sine)
			{
				requireSmoothSine(rms, roughness, 60);
				require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
					static_cast<uint8_t>(0xb0 | track), 82, 64)), "SRR change rejected");
				double reducedRoughness = 0;
				const auto reduced = render(hardware, &reducedRoughness);
				std::cout << "reduced-rate RMS " << reduced << ", roughness " << reducedRoughness << '\n';
				require(reduced > 1e-5 && reducedRoughness > roughness * 2,
					"GND SIN sample-rate reduction had no observable effect");
				require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
					static_cast<uint8_t>(0xb0 | track), 82, 0)), "SRR restore rejected");
				// Burst parameter traffic across both DSPs while this track sounds.
				// Finish at nominal SRR on every voice, then require the audible DSP
				// state to agree. A MIDI/kit-status response alone would not prove it.
				for(unsigned pass = 0; pass < 16; ++pass)
					for(uint8_t voice = 0; voice < 6; ++voice)
						require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
							static_cast<uint8_t>(0xb0 | voice), 82, (pass & 1) ? 0 : 64)),
							"SRR burst rejected");
				advance(hardware, md::g_samplerate / 4);
				double restoredRoughness = 0;
				const auto restored = render(hardware, &restoredRoughness);
				std::cout << "restored-rate RMS " << restored << ", roughness " << restoredRoughness << '\n';
				requireSmoothSine(restored, restoredRoughness, 60);
			}
			require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
				static_cast<uint8_t>(0x80 | track), 60, 0)), "note-off rejected");
			advance(hardware, md::g_samplerate);
			if(sine)
				for(const uint8_t note : {36, 48, 72, 84})
				{
					require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
						static_cast<uint8_t>(0x90 | track), note, 100)), "sine sweep note rejected");
					double sweepRoughness = 0;
					const auto sweepRms = render(hardware, &sweepRoughness);
					std::cout << "sine note " << unsigned(note) << " RMS " << sweepRms
						<< ", roughness " << sweepRoughness << '\n';
					requireSmoothSine(sweepRms, sweepRoughness, note);
					require(hardware.sendMidi(synthLib::SMidiEvent(synthLib::MidiEventSource::Host,
						static_cast<uint8_t>(0x80 | track), note, 0)), "sine sweep note-off rejected");
					advance(hardware, md::g_samplerate / 2);
				}
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
