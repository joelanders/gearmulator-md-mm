#include "mdLib/mdhardware.h"
#include "sdsTestData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
	std::vector<uint8_t> load(const char* path)
	{
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}
	void advance(md::Hardware& hardware, uint32_t frames)
	{
		while(frames) { const auto n = std::min<uint32_t>(frames, 64); hardware.advance(n); frames -= n; }
	}

	bool boot(md::Hardware& hardware)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while(!hardware.isFirmwareMidiReady())
		{
			hardware.advance(64);
			if(std::chrono::steady_clock::now() >= deadline) return false;
		}
		advance(hardware, md::g_samplerate * 20);
		return true;
	}

	// Independent decoding to the signed big-endian 16-bit words stored by OS
	// 1.63. No flash addresses or firmware-private structures enter this oracle.
	std::vector<std::vector<uint8_t>> expectedSamples(const std::vector<uint8_t>& bytes)
	{
		std::vector<std::vector<uint8_t>> samples;
		size_t words = 0, width = 0;
		for(size_t offset = 0; offset < bytes.size();)
		{
			const auto end = std::find(bytes.begin() + offset, bytes.end(), uint8_t{0xf7});
			if(end == bytes.end()) return {};
			const auto* b = bytes.data() + offset;
			if(b[1] == 0x7e && b[3] == 1)
			{
				words = b[10] | (size_t(b[11]) << 7) | (size_t(b[12]) << 14);
				width = (b[6] + 6) / 7;
				samples.emplace_back();
			}
			else if(b[1] == 0x7e && b[3] == 2)
			{
				for(size_t i = 5; i + width <= 125 && words; i += width, --words)
				{
					uint32_t value = 0;
					for(size_t j = 0; j < width; ++j) value = (value << 7) | b[i+j];
					value = width * 7 >= 16 ? value >> (width * 7 - 16) : value << (16 - width * 7);
					value ^= 0x8000;
					samples.back().push_back((value >> 8) & 0xff);
					samples.back().push_back(value & 0xff);
				}
			}
			offset = end - bytes.begin() + 1;
		}
		return samples;
	}

	bool containsSamples(const std::vector<uint8_t>& flash, const std::vector<std::vector<uint8_t>>& samples)
	{
		for(const auto& sample : samples)
		{
			const auto found = std::search(flash.begin(), flash.end(), sample.begin(), sample.end());
			if(sample.empty() || found == flash.end())
			{
				std::puts("Expected complete sample contents missing from flash");
				const auto prefix = std::search(flash.begin(), flash.end(), sample.begin(), sample.begin() + std::min<size_t>(64, sample.size()));
				if(prefix != flash.end())
				{
					size_t i = 0;
					while(i < sample.size() && prefix + i != flash.end() && prefix[i] == sample[i]) ++i;
					std::printf("Prefix at %zx matches %zu/%zu bytes\n", size_t(prefix-flash.begin()), i, sample.size());
				}
				return false;
			}
			std::printf("Verified %zu sample words at flash offset %zx\n", sample.size()/2, size_t(found-flash.begin()));
		}
		return !samples.empty();
	}

	std::vector<uint8_t> readKit(md::Hardware& hardware)
	{
		std::vector<synthLib::SMidiEvent> events;
		hardware.readMidiOut(events);
		synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
		request.sysex = {0xf0, 0, 0x20, 0x3c, 2, 0, 0x53, 0, 0xf7};
		if(!hardware.sendMidi(request)) return {};
		for(size_t i = 0; i < md::g_samplerate * 3 / 64; ++i)
		{
			hardware.advance(64);
			events.clear();
			hardware.readMidiOut(events);
			for(const auto& event : events)
				if(event.sysex.size() > 30 && event.sysex[1] == 0 && event.sysex[4] == 2
					&& event.sysex[6] == 0x52) return event.sysex;
		}
		return {};
	}

	bool checkPlayback(md::Hardware& hardware)
	{
		advance(hardware, md::g_samplerate * 15);
		synthLib::SMidiEvent assign(synthLib::MidiEventSource::Host);
		assign.sysex = {0xf0, 0, 0x20, 0x3c, 2, 0, 0x5b, 0, 0, 1, 0xf7};
		if(!hardware.sendMidi(assign)) return false;
		advance(hardware, 8192);
		const std::array<uint8_t, 6> values{64, 127, 127, 0, 0, 127};
		for(size_t i = 0; i < values.size(); ++i)
			if(!hardware.sendMidi({synthLib::MidiEventSource::Host, synthLib::M_CONTROLCHANGE,
				uint8_t(0x10 + i), values[i]})) return false;
		advance(hardware, 8192);
		const auto trigger = md::panelPacket(md::MachineModel::Machinedrum, md::PanelControl::Trigger1);
		if(!trigger) return false;
		if(const auto* path = std::getenv("MD_SDS_PANEL_PROBE"))
		{
			const auto panel = hardware.getFrontPanelSnapshot();
			std::ofstream image(path, std::ios::binary);
			image << "P5\n128 64\n255\n";
			for(uint32_t y = 0; y < 64; ++y)
				for(uint32_t x = 0; x < 128; ++x) image.put(panel.getLcdPixel(x, y) ? '\0' : '\xff');
		}
		std::array<std::vector<float>, 2> audio{std::vector<float>(8192), std::vector<float>(8192)};
		synthLib::TAudioOutputs outputs{};
		for(size_t c = 0; c < audio.size(); ++c) outputs[c] = audio[c].data();
		hardware.sendPanelEvent(trigger->row, trigger->mask);
		hardware.processAudio(outputs, 4096, 0);
		hardware.sendPanelEvent(trigger->row, 0);
		for(size_t c = 0; c < audio.size(); ++c) outputs[c] += 4096;
		hardware.processAudio(outputs, 4096, 0);
		double best = 0, peak = 0;
		for(const auto& channel : audio)
		{
			for(auto sample : channel)
			{
				if(!std::isfinite(sample)) return false;
				peak = std::max(peak, std::abs(double(sample)));
			}
			constexpr size_t window = 1024;
			for(size_t begin = 0; begin + window <= channel.size(); begin += 512)
				for(size_t lag = 0; lag < 140; ++lag)
				{
					double x = 0, y = 0, xx = 0, yy = 0, xy = 0;
					for(size_t i = 0; i < window; ++i)
					{
						const double reference = std::fmod((i + lag) * 32000.0 / md::g_samplerate, 97.0) / 97.0;
						const double sample = channel[begin + i];
						x += reference; y += sample; xx += reference * reference;
						yy += sample * sample; xy += reference * sample;
					}
					const double variance = (xx-x*x/window) * (yy-y*y/window);
					if(variance > 0) best = std::max(best, (xy-x*y/window) / std::sqrt(variance));
				}
		}
		std::printf("Sample playback: peak=%.6f correlation=%.6f\n", peak, best);
		return peak > 0.001 && best > 0.85;
	}

}

int main(int argc, char** argv)
{
	if(argc != 4)
	{
		std::puts("usage: mdSdsFirmwareTest <MD-ROM> <sample.syx|--generated|--generated-bank|--generated-mixed|--generated-cancel> <factory.cache>");
		return 2;
	}
	const auto rom = load(argv[1]);
	const auto cache = load(argv[3]);
	const bool generated = std::string(argv[2]).find("--generated") == 0;
	const bool mixed = std::string(argv[2]) == "--generated-mixed";
	const bool cancelTest = std::string(argv[2]) == "--generated-cancel";
	if(generated && !mixed && !cancelTest && std::string(argv[2]) != "--generated"
		&& std::string(argv[2]) != "--generated-bank") return 2;
	auto bytes = generated ? md::test::sdsSample() : load(argv[2]);
	if(std::string(argv[2]) == "--generated-bank" || mixed)
	{
		const auto second = md::test::sdsSample(4097, 12, 4, 1);
		bytes.insert(bytes.end(), second.begin(), second.end());
	}
	md::MidiSysexStreamValidation validation{};
	auto prepared = md::prepareMidiSysexTransfer(bytes, md::MachineModel::Machinedrum, &validation);
	if(!prepared) { std::puts(md::midiSysexValidationMessage(validation)); return 2; }
	const auto samples = expectedSamples(bytes);
	std::vector<uint8_t> baseline;
	if(rom.empty() || !md::decodeFactoryFlashCache(baseline, cache, rom)) return 2;
	auto machine = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum,
		std::vector<uint8_t>{}, std::shared_ptr<md::FrontPanelPublisher>{}, std::vector<uint8_t>{}, cache);
	auto& hardware = *machine;
	if(!hardware.isValid() || !boot(hardware)) return 1;
	std::vector<uint8_t> expectedKit;
	if(mixed)
	{
		expectedKit = readKit(hardware);
		if(expectedKit.empty()) { std::puts("Could not obtain mixed-file kit fixture"); return 1; }
		const std::string name = "SDS MIXED TEST  ";
		std::copy(name.begin(), name.end(), expectedKit.begin() + 10);
		const auto sumPos = expectedKit.size() - 5;
		uint32_t sum = 0;
		for(size_t i = 9; i < sumPos; ++i) sum += expectedKit[i];
		expectedKit[sumPos] = (sum >> 7) & 0x7f;
		expectedKit[sumPos + 1] = sum & 0x7f;
		bytes.insert(bytes.end(), expectedKit.begin(), expectedKit.end());
		prepared = md::prepareMidiSysexTransfer(bytes);
		if(!prepared) return 1;
	}
	if(!hardware.startMidiSysexTransfer(*prepared)) return 1;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
	uint8_t payloadSpeed = 1;
	bool cancelled = false;
	for(;;)
	{
		hardware.advance(64);
		const auto progress = hardware.getMidiSysexTransferProgress();
		if(progress.state == md::MidiSysexTransferState::Sending
			|| progress.state == md::MidiSysexTransferState::WaitingForDevice
			|| progress.state == md::MidiSysexTransferState::Retrying)
			payloadSpeed = std::max(payloadSpeed, progress.speedCode);
		if(progress.state == md::MidiSysexTransferState::WaitingForReceiveMode)
		{
			// Stand in for the user waiting for CLEANING/LOADING, then Resume.
			advance(hardware, md::g_samplerate * 20);
			if(!hardware.resumeMidiSysexReceiveMode(progress.transferId, progress.receiveStep)) return 1;
		}
		if(cancelTest && !cancelled && progress.sent >= 300 && progress.sent < progress.total)
		{
			std::vector<uint8_t> retired;
			if(!hardware.cancelMidiSysexTransfer(retired) || retired != bytes) return 1;
			advance(hardware, md::g_samplerate * 20);
			if(hardware.getMidiSysexTransferProgress().state != md::MidiSysexTransferState::Cancelled
				|| !hardware.isMidiIngressIdle()) return 1;
			prepared = md::prepareMidiSysexTransfer(bytes);
			if(!prepared || !hardware.startMidiSysexTransfer(*prepared)) return 1;
			cancelled = true;
			std::puts("Cancelled mid-sample, drained MIDI, and started a fresh import");
		}
		if(progress.state == md::MidiSysexTransferState::Complete) break;
		if(progress.state == md::MidiSysexTransferState::Failed || std::chrono::steady_clock::now() >= deadline)
		{
			std::printf("SDS failed: state=%u error=%u sent=%zu/%zu retries=%u\n",
				unsigned(progress.state), unsigned(progress.error), progress.sent, progress.total, progress.retries);
			return 1;
		}
	}
	advance(hardware, md::g_samplerate * 5);
	if(mixed)
	{
		const auto kit = readKit(hardware);
		if(kit.size() != expectedKit.size() || !std::equal(expectedKit.begin() + 10,
			expectedKit.begin() + 25, kit.begin() + 10))
		{ std::puts("Kit following SDS did not import"); return 1; }
		std::puts("Verified kit following sample bank through firmware dump request");
	}
	const auto flash = hardware.copyFlashData();
	if(!containsSamples(flash, samples)) return 1;
	const auto progress = hardware.getMidiSysexTransferProgress();
	if(progress.acknowledgedSamples != samples.size()) return 1;
	std::vector<uint8_t> state;
	md::DecodedState decoded;
	if(!md::encodeStateWithFactoryBaseline(state, hardware.copyPatchRam(), flash,
		baseline, rom, md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
		|| !md::decodeState(decoded, state, rom, md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)) return 1;
	auto restoredFlash = baseline;
	if(!md::applyFlashOverlay(restoredFlash, decoded.flashOverlay, baseline, rom) || restoredFlash != flash) return 1;
	// The mixed fixture imports a stored kit, whose routing/mutes need not play
	// track 1. The separate generated bank verifies audio before/after reload.
	if(generated && !mixed && !checkPlayback(hardware)) return 1;
	machine.reset();
	auto restored = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum,
		decoded.patchRam, std::shared_ptr<md::FrontPanelPublisher>{}, restoredFlash, cache);
	if(!restored->isValid() || !boot(*restored)
		|| !containsSamples(restored->copyFlashData(), samples)
		|| (generated && !mixed && !checkPlayback(*restored))) return 1;
	std::printf("SDS firmware acceptance passed: bytes=%zu samples=%u retries=%u speed=%sx stateBytes=%zu\n",
		progress.sent, progress.acknowledgedSamples, progress.retries, md::midiTurboSpeedLabel(payloadSpeed), state.size());
	return 0;
}
