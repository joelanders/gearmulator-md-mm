#include "sysexContentOracle.h"
#include "sysexPanelDriver.h"
#include "digiproAudioOracle.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <memory>
#include <limits>

namespace
{
	using namespace md::test;
	Sysex load(const char* path)
	{
		std::ifstream stream(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}
	void boot(md::Hardware& hardware)
	{
		advanceFrames(hardware, md::g_samplerate * 20);
		require(hardware.isFirmwareMidiReady(), "MM boot incomplete");
	}
	void send(md::Hardware& hardware, const Sysex& bytes, unsigned cancelBoundary = 0)
	{
		unsigned boundaries = 0;
		const auto messages = splitSysex(bytes);
		const unsigned expectedBoundaries = messages.front()[6] == 0x52 ? 2 + cancelBoundary : 0;
		uint32_t cancelledId = 0;
		size_t cancelledStep = 0;
		auto prepared = md::prepareMidiSysexTransfer(bytes, md::MachineModel::Monomachine);
		require(prepared && hardware.startMidiSysexTransfer(*prepared), "MM transfer start failed");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
		while(std::chrono::steady_clock::now() < deadline)
		{
			hardware.advance(nextSysexTestBlockSize());
			const auto p = hardware.getMidiSysexTransferProgress();
			require(p.state != md::MidiSysexTransferState::Failed, "MM transfer failed");
			if(p.state == md::MidiSysexTransferState::Complete)
			{
				require(boundaries == expectedBoundaries, "expected receive-mode boundaries were not exercised");
				return;
			}
			if(p.state == md::MidiSysexTransferState::WaitingForReceiveMode)
			{
				++boundaries;
				if(cancelledId)
					require(!hardware.resumeMidiSysexReceiveMode(cancelledId, cancelledStep), "old transfer resumed a new paused import");
				require(!hardware.resumeMidiSysexReceiveMode(p.transferId + 1, p.receiveStep)
					&& !hardware.resumeMidiSysexReceiveMode(p.transferId, p.receiveStep + 1), "stale resume accepted");
				advanceFrames(hardware, md::g_samplerate * 2);
				require(hardware.getMidiSysexTransferProgress().sent == p.sent, "payload leaked across receive-mode pause");
				std::printf("MM BOUNDARY %u kind=%u step=%zu sent=%zu\n", boundaries, unsigned(p.receiveKind), p.receiveStep, p.sent);
				if(cancelBoundary == boundaries)
				{
					cancelledId = p.transferId;
					cancelledStep = p.receiveStep;
					Sysex retired;
					require(hardware.cancelMidiSysexTransfer(retired) && retired == bytes, "boundary cancellation failed");
					advanceFrames(hardware, md::g_samplerate * 2);
					require(hardware.getMidiSysexTransferProgress().state == md::MidiSysexTransferState::Cancelled
						&& hardware.isMidiIngressIdle(), "cancel did not release MIDI");
					require(!hardware.resumeMidiSysexReceiveMode(p.transferId, p.receiveStep), "cancelled resume accepted");
					exitMenus(hardware);
					advanceFrames(hardware, md::g_samplerate * 3);
					enterMmReceive(hardware, false);
					prepared = md::prepareMidiSysexTransfer(bytes, md::MachineModel::Monomachine);
					require(prepared && hardware.startMidiSysexTransfer(*prepared), "restart after cancellation failed");
					require(!hardware.resumeMidiSysexReceiveMode(p.transferId, p.receiveStep), "retired transfer resume accepted");
					cancelBoundary = 0;
					continue;
				}
				exitMenus(hardware);
				advanceFrames(hardware, md::g_samplerate * 3);
				enterMmReceive(hardware, p.receiveKind == md::MidiSysexMessageKind::DigiPro);
				require(hardware.resumeMidiSysexReceiveMode(p.transferId, p.receiveStep), "valid resume rejected");
				require(!hardware.resumeMidiSysexReceiveMode(p.transferId, p.receiveStep), "duplicate resume accepted");
			}
		}
		require(false, "MM transfer deadline expired");
	}
	Sysex unpackKit(const Sysex& message)
	{
		require(message.size() > 15, "empty kit dump");
		const auto packed = unpackElektron(message.begin() + 10, message.end() - 5);
		Sysex result;
		for(size_t i = 0; i < packed.size(); ++i)
		{
			if(!(packed[i] & 128)) result.push_back(packed[i]);
			else
			{
				const auto count = packed[i] & 127;
				require(count && i + 1 < packed.size(), "invalid MM run length");
				result.insert(result.end(), count, packed[++i]);
			}
		}
		return result;
	}
	Sysex namedKit(md::Hardware& hardware, uint8_t slot, const std::string& name)
	{
		const auto original = requestDump(hardware, {0xf0, 0, 0x20, 0x3c, 3, 0, 0x52, 2, 1, 0});
		auto raw = unpackKit(original);
		require(raw.size() > 11 && name.size() <= 11, "kit name fixture invalid");
		std::fill_n(raw.begin(), 11, 0);
		std::copy(name.begin(), name.end(), raw.begin());
		Sysex packed;
		// Deliberately use literal RLE encoding, not firmware's compression choices.
		for(auto b : raw) { if(b & 128) packed.push_back(0x81); packed.push_back(b); }
		Sysex message(original.begin(), original.begin() + 10);
		message[9] = slot;
		for(size_t i = 0; i < packed.size();)
		{
			const size_t n = std::min<size_t>(7, packed.size() - i);
			uint8_t high = 0;
			for(size_t j = 0; j < n; ++j) high |= ((packed[i+j] >> 7) << (6-j));
			message.push_back(high);
			for(size_t j = 0; j < n; ++j) message.push_back(packed[i++] & 127);
		}
		unsigned sum = 0;
		for(size_t i = 9; i < message.size(); ++i) sum += message[i];
		const auto length = message.size() - 5;
		message.insert(message.end(), {uint8_t((sum >> 7) & 127), uint8_t(sum & 127),
			uint8_t((length >> 7) & 127), uint8_t(length & 127), 0xf7});
		return message;
	}
	void verifyKits(md::Hardware& hardware, const Sysex& file)
	{
		for(const auto& message : splitSysex(file))
			if(message[6] == 0x52)
			{
				require(unpackKit(requestDump(hardware, message)) == unpackKit(message), "mixed kit payload mismatch");
				std::printf("MM MIXED KIT slot=%u complete decoded payload matches\n", message[9]);
			}
	}
	void cc(md::Hardware& hardware, uint8_t controller, uint8_t value)
	{
		require(hardware.sendMidi({synthLib::MidiEventSource::Host, 0xb0, controller, value}), "CC rejected");
	}
	std::vector<float> renderWave(md::Hardware& hardware, uint8_t select)
	{
		hardware.sendMidi({synthLib::MidiEventSource::Host, 0x80, 48, 0});
		cc(hardware, 48, select); cc(hardware, 50, select);
		cc(hardware, 49, 0); cc(hardware, 51, 0); // mix first wave; no glide
		advanceFrames(hardware, md::g_samplerate / 2);
		if(const auto* prefix = std::getenv("MM_AUDIO_DIAGNOSTIC"))
			panelImage(hardware, std::string(prefix) + "-" + std::to_string(select) + ".pgm");
		require(hardware.sendMidi({synthLib::MidiEventSource::Host, 0x90, 48, 100}), "note rejected");
		std::array<std::vector<float>, 2> channels{std::vector<float>(16384), std::vector<float>(16384)};
		synthLib::TAudioOutputs output{};
		output[0] = channels[0].data(); output[1] = channels[1].data();
		if(std::getenv("MM_SYSEX_BLOCK_PROFILE"))
		{
			for(uint32_t offset = 0; offset < 16384;)
			{
				const auto n = std::min<uint32_t>(16384 - offset, nextSysexTestBlockSize());
				output[0] = channels[0].data() + offset;
				output[1] = channels[1].data() + offset;
				hardware.processAudio(output, n, 0);
				offset += n;
			}
		}
		else hardware.processAudio(output, 16384, 0);
		for(const auto& channel : channels)
			for(auto sample : channel) require(std::isfinite(sample), "non-finite audio");
		if(const auto* prefix = std::getenv("MM_AUDIO_DIAGNOSTIC"))
		{
			std::ofstream out(std::string(prefix) + "-" + std::to_string(select) + ".f32", std::ios::binary);
			out.write(reinterpret_cast<const char*>(channels[0].data()), channels[0].size() * sizeof(float));
		}
		return channels[0];
	}
	void oracleSelfTest()
	{
		const std::string profile = std::getenv("MM_SYSEX_BLOCK_PROFILE") ? std::getenv("MM_SYSEX_BLOCK_PROFILE") : "64";
		const std::array<uint32_t, 7> irregular{1, 17, 63, 128, 511, 1024, 3};
		for(size_t i = 0; i < irregular.size() * 2; ++i)
			require(nextSysexTestBlockSize() == (profile == "irregular" ? irregular[i % irregular.size()] : std::stoul(profile)),
				"block profile sequence differs");
		constexpr double tau = 6.2831853071795864769;
		const auto reference = [](double phase) { return 0.5*std::sin(tau*phase) + 0.2*std::cos(tau*phase*3) + 0.1*std::sin(tau*phase*7); };
		Sysex wave(6132);
		for(size_t i = 0; i < 1024; ++i)
		{
			const auto raw = uint32_t(int32_t(reference(i / 1024.0) * 0x7fffff));
			wave[i*3] = (raw >> 16) & 255; wave[i*3+1] = (raw >> 8) & 255; wave[i*3+2] = raw & 255;
		}
		std::vector<float> audio(16384);
		const double frequency = 440.0 * std::pow(2.0, (48.0 - 69.0) / 12.0);
		for(size_t i = 0; i < audio.size(); ++i) audio[i] = float(reference(i * frequency / 44100 + 0.137) * 0.4 + 0.01);
		require(digiProAudioMatches(audio, wave), "oracle rejected reference with gain/DC/phase changes");
		require(!digiProAudioMatches(audio, Sysex(6132)), "oracle accepted erased reference");
		require(!digiProAudioMatches(audio, Sysex(6131)), "oracle accepted truncated reference");
		for(float value : {0.0f, 0.1f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
			require(!digiProAudioMatches(std::vector<float>(16384, value), wave), "oracle accepted silent/DC/non-finite audio");
		for(float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
		{
			auto broken = audio;
			broken.back() = value;
			require(!digiProAudioMatches(broken, wave), "oracle missed isolated non-finite sample outside correlation window");
		}
		for(size_t i = 0; i < audio.size(); ++i) audio[i] = float(reference(i * frequency * 1.1 / 44100));
		require(!digiProAudioMatches(audio, wave), "oracle accepted wrong pitch");
		for(size_t i = 0; i < audio.size(); ++i) audio[i] = float(std::cos(tau * i * frequency * 11 / 44100));
		require(!digiProAudioMatches(audio, wave), "oracle accepted different periodic waveform");
		uint32_t random = 42;
		for(auto& a : audio) { random = random*1664525u + 1013904223u; a = float(double(random)/0xffffffffu - 0.5); }
		require(!digiProAudioMatches(audio, wave), "oracle accepted broadband noise");
		std::puts("DigiPRO audio oracle positive and negative controls passed");
	}
	void playback(md::Hardware& hardware, const Sysex& file, const char* label)
	{
		emptyMmKit(hardware);
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex = {0xf0, 0, 0x20, 0x3c, 3, 0, 0x5b, 0, 32, 1, 0xf7};
		require(hardware.sendMidi(assign), "DPRO assignment failed");
		advanceFrames(hardware, md::g_samplerate);
		cc(hardware, 7, 127);
		const auto messages = splitSysex(file);
		bool matches = true;
		for(size_t slot : {0, 1, 45, 63})
		{
			const auto found = std::find_if(messages.begin(), messages.end(), [slot](const auto& m) { return m[9] == slot; });
			require(found != messages.end(), "playback fixture must include slots 0, 1, 45, 63");
			const auto wave = unpackElektron(found->begin() + 14, found->end() - 5);
			// OS 1.32b scales the MIDI range onto D01..D64. Odd values select
			// these slots reliably; value 2 still displays D01, not D02.
			const auto audio = renderWave(hardware, uint8_t(slot * 2 + 1));
			double energy = 0;
			for(auto a : audio) energy += a*a;
			const double score = digiProCorrelation(audio, wave);
			std::printf("PLAYBACK %s slot=%zu rms=%.7f correlation=%.7f\n", label, slot, std::sqrt(energy/audio.size()), score);
			matches &= energy > 0.0001 && score > 0.90;
			if(std::getenv("MM_AUDIO_DIAGNOSTIC"))
				for(const auto& other : messages)
					if(other[9] == 0 || other[9] == 1 || other[9] == 45 || other[9] == 63)
						std::printf("AUDIO CROSS requested=%zu reference=%u score=%.7f\n", slot, other[9],
							digiProCorrelation(audio, unpackElektron(other.begin() + 14, other.end() - 5)));
		}
		require(matches, "imported waveform audio does not match decoded reference");
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if(argc == 2 && std::string(argv[1]) == "--audio-oracle")
	{
		try { oracleSelfTest(); return 0; }
		catch(const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
	}
	if(argc != 4 && argc != 5) { std::puts("usage: mmSysexWorkflowTest <ROM> <patch-ram> <DigiPRO.syx> [mixed|cancel-before-digipro|cancel-before-general]"); return 2; }
	try
	{
		const auto rom = load(argv[1]), patch = load(argv[2]), file = load(argv[3]);
		(void)nextSysexTestBlockSize(); // Validate profile before booting.
		std::printf("MM WORKFLOW blockProfile=%s boundedJit=%s\n",
			std::getenv("MM_SYSEX_BLOCK_PROFILE") ? std::getenv("MM_SYSEX_BLOCK_PROFILE") : "default",
			std::getenv("GEARMULATOR_MDMM_BOUNDED_JIT") ? std::getenv("GEARMULATOR_MDMM_BOUNDED_JIT") : "default");
		require(md::validateMidiSysexStream(file, md::MachineModel::Monomachine) == md::MidiSysexStreamValidation::Valid, "bad input");
		std::array<bool, 64> slots{};
		const auto waves = splitSysex(file);
		require(waves.size() == slots.size(), "fixture must be a complete 64-wave DigiPRO bank");
		for(const auto& wave : waves)
		{
			require(wave.size() == 7027 && wave[6] == 0x5d && wave[9] < slots.size() && !slots[wave[9]], "invalid/duplicate wave fixture");
			slots[wave[9]] = true;
		}
		auto machine = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Monomachine, patch);
		boot(*machine);
		Sysex transfer = file;
		if(argc == 5)
		{
			transfer = namedKit(*machine, 62, "BEFORE WAVE");
			for(const auto& message : splitSysex(file))
				if(message[9] == 0 || message[9] == 1 || message[9] == 45 || message[9] == 63)
					transfer.insert(transfer.end(), message.begin(), message.end());
			const auto last = namedKit(*machine, 63, "AFTER WAVE");
			transfer.insert(transfer.end(), last.begin(), last.end());
		}
		const std::string mode = argc == 5 ? argv[4] : "";
		require(mode.empty() || mode == "mixed" || mode == "cancel-before-digipro" || mode == "cancel-before-general", "unknown mode");
		enterMmReceive(*machine, argc == 4);
		send(*machine, transfer, mode == "cancel-before-digipro" ? 1 : mode == "cancel-before-general" ? 2 : 0);
		panelTap(*machine, md::PanelControl::Exit);
		advanceFrames(*machine, md::g_samplerate * 20);
		exitMenus(*machine);
		if(const auto* prefix = std::getenv("MM_SYSEX_DIAGNOSTIC"))
		{
			panelImage(*machine, std::string(prefix) + "-final.pgm");
			const auto flash = machine->copyFlashData();
			std::ofstream output(std::string(prefix) + "-flash.bin", std::ios::binary);
			output.write(reinterpret_cast<const char*>(flash.data()), std::streamsize(flash.size()));
			std::printf("MM DIAGNOSTIC midiConsumed=%llu overflow=%zu ingressIdle=%u panelPending=%zu panelOverflow=%zu\n",
				static_cast<unsigned long long>(machine->midiRxConsumedCount()), machine->midiRxOverflowCount(),
				machine->isMidiIngressIdle(), machine->getPendingPanelInputBytes(), machine->getPanelInputOverflowCount());
		}
		verifyKits(*machine, transfer);
		require(verifyDigiProContents(transfer, machine->copyFlashData()), "import contents mismatch");
		Sysex state;
		md::DecodedState decoded;
		require(md::encodeState(state, machine->copyPatchRam(), md::MachineModel::Monomachine,
			synthLib::StateTypeGlobal, machine->copyUserFlash()) && md::decodeState(decoded, state, {},
			md::MachineModel::Monomachine, synthLib::StateTypeGlobal), "state codec failed");
		playback(*machine, file, "imported");
		machine.reset();
		machine = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Monomachine, decoded.patchRam,
			std::shared_ptr<md::FrontPanelPublisher>{}, Sysex{}, Sysex{}, md::FlashSectorOverlay{}, decoded.userFlash);
		boot(*machine);
		verifyKits(*machine, transfer);
		require(verifyDigiProContents(transfer, machine->copyFlashData()), "booted restored wave contents mismatch");
		playback(*machine, file, "reloaded");
		return 0;
	}
	catch(const std::exception& error) { std::fprintf(stderr, "MM WORKFLOW FAIL: %s\n", error.what()); return 1; }
}
