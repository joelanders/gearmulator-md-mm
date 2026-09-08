#include "mdLib/mdhardware.h"
#include "sdsTestData.h"
#include "sdsFaultWire.h"
#include "sysexReadinessTrace.h"

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
	std::unique_ptr<md::test::ReadinessTrace> readinessTrace;
	void step(md::Hardware& hardware, uint32_t frames)
	{
		hardware.advance(frames);
		if(readinessTrace) readinessTrace->observe(hardware);
	}
	std::vector<uint8_t> load(const char* path)
	{
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}
	void advance(md::Hardware& hardware, uint32_t frames)
	{
		while(frames) { const auto n = std::min<uint32_t>(frames, 64); step(hardware, n); frames -= n; }
	}

	bool boot(md::Hardware& hardware, uint32_t settleSeconds = 20)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while(!hardware.isFirmwareMidiReady())
		{
			step(hardware, 64);
			if(std::chrono::steady_clock::now() >= deadline) return false;
		}
		advance(hardware, md::g_samplerate * settleSeconds);
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
			step(hardware, 64);
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

	bool faultTest(md::Hardware& hardware, const std::vector<uint8_t>& bytes,
		const std::vector<std::vector<uint8_t>>& samples, const std::string& mode,
		const std::vector<uint8_t>& baseline, const std::vector<uint8_t>& rom)
	{
		if(mode != "none" && mode != "drop-ack" && mode != "silence" && mode != "delay-ack"
			&& mode != "duplicate-ack" && mode != "wait" && mode != "corrupt-packet"
			&& mode != "drop-header-ack" && mode != "drop-final-ack") return false;
		md::TurboMidiTransfer transfer(40'000'000);
		md::test::SdsFaultWire wire(hardware, transfer, mode);
		auto prepared = md::prepareMidiSysexTransfer(bytes);
		if(!prepared || !transfer.start(*prepared, 0)) return false;
		const auto run = [&]()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while(transfer.ownsMidiWire() && std::chrono::steady_clock::now() < deadline)
			{
				const auto before = hardware.getUC().getCycles();
				hardware.advance(1);
				const auto elapsed = hardware.getUC().getCycles() - before;
				wire.tick(elapsed);
				transfer.service(uint32_t(elapsed), true, wire);
			}
		};
		run();
		const auto progress = transfer.progress();
		// Keep delayed replies scheduled even if the transfer completed before
		// their arrival. A late ACK must really reach the now-idle sender.
		for(uint32_t frames = md::g_samplerate * 20; frames;)
		{
			const auto n = std::min<uint32_t>(frames, 64);
			const auto before = hardware.getUC().getCycles();
			hardware.advance(n);
			wire.tick(hardware.getUC().getCycles() - before);
			frames -= n;
		}
		const auto flash = hardware.copyFlashData();
		const bool contents = containsSamples(flash, samples);
		std::printf("FAULT RESULT mode=%s state=%u error=%u retries=%u injected=%zu naks=%zu released=%zu contents=%u acked=%u\n",
			mode.c_str(), unsigned(progress.state), unsigned(progress.error), progress.retries,
			wire.injected(), wire.naks(), wire.released(), contents, progress.acknowledgedSamples);
		if(mode != "none" && !wire.injected()) return false;
		if((mode == "wait" || mode == "delay-ack") && wire.released() != 1) return false;
		if(mode == "silence" || mode == "drop-final-ack" || mode == "drop-header-ack")
		{
			const auto error = mode == "drop-header-ack" ? md::MidiSysexTransferError::ReplyTimedOut
				: md::MidiSysexTransferError::RetryLimit;
			if(progress.state != md::MidiSysexTransferState::Failed || progress.error != error
				|| transfer.ownsMidiWire() || progress.acknowledgedSamples != 0
				|| hardware.queuedMidiRxBytes() != 0 || contents != (mode == "drop-final-ack")) return false;
			wire.disableFault();
			const auto recovery = md::test::sdsSample(4097, 12, 0, 1);
			prepared = md::prepareMidiSysexTransfer(recovery);
			if(!prepared || !transfer.start(*prepared, 0)) return false;
			run();
			advance(hardware, md::g_samplerate * 20);
			const bool recovered = transfer.progress().state == md::MidiSysexTransferState::Complete
				&& transfer.progress().acknowledgedSamples == 1
				&& containsSamples(hardware.copyFlashData(), expectedSamples(recovery));
			std::printf("FAULT RECOVERY mode=%s success=%u\n", mode.c_str(), recovered);
			return recovered;
		}
		std::vector<uint8_t> state;
		md::DecodedState decoded;
		auto restoredFlash = baseline;
		return progress.state == md::MidiSysexTransferState::Complete && contents
			&& progress.acknowledgedSamples == samples.size()
			&& md::encodeStateWithFactoryBaseline(state, hardware.copyPatchRam(), flash, baseline, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
			&& md::decodeState(decoded, state, rom, md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
			&& md::applyFlashOverlay(restoredFlash, decoded.flashOverlay, baseline, rom)
			&& restoredFlash == flash && containsSamples(restoredFlash, samples);
	}

	md::MidiSysexTransferProgress runImport(md::Hardware& hardware,
		const std::vector<uint8_t>& bytes, bool cancel = false)
	{
		auto prepared = md::prepareMidiSysexTransfer(bytes);
		if(!prepared || !hardware.startMidiSysexTransfer(*prepared)) return {};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while(std::chrono::steady_clock::now() < deadline)
		{
			step(hardware, 64);
			const auto progress = hardware.getMidiSysexTransferProgress();
			if(cancel && progress.sent >= 300 && progress.sent < progress.total)
			{
				std::vector<uint8_t> retired;
				if(!hardware.cancelMidiSysexTransfer(retired)) return {};
				cancel = false;
			}
			if(progress.state == md::MidiSysexTransferState::Complete
				|| progress.state == md::MidiSysexTransferState::Failed
				|| progress.state == md::MidiSysexTransferState::Cancelled) return progress;
		}
		return hardware.getMidiSysexTransferProgress();
	}

	bool readinessTest(md::Hardware& hardware, const std::string& mode, uint32_t delay)
	{
		if(mode == "observe" || mode == "observe-restored")
		{
			advance(hardware, md::g_samplerate * 20);
			std::puts("READINESS CONTROL no MIDI import sent");
			return true;
		}
		if(mode == "quiet" || mode == "pc")
		{
			// Explicitly test candidate heuristics; never install either as a
			// production readiness guarantee. PC is a sampled stock-1.63 location
			// seen both during startup and later steady-state processing.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
			while(mode == "quiet" ? hardware.getUC().flashIdleCycles() < uint64_t(delay)*40'000'000
				: hardware.getUC().getPC() != 0x201124)
			{
				step(hardware, 64);
				if(std::chrono::steady_clock::now() >= deadline) return false;
			}
		}
		if(mode == "probe")
		{
			const auto start = hardware.getUC().getCycles();
			bool replied = false;
			for(unsigned attempt = 0; attempt < 10 && !replied; ++attempt) replied = !readKit(hardware).empty();
			std::printf("READINESS PROBE replied=%u emulatedSeconds=%.3f\n", replied,
				double(hardware.getUC().getCycles() - start) / 40'000'000);
			if(!replied) return false;
		}
		const auto first = md::test::sdsSample();
		const auto second = md::test::sdsSample(4097, 12, 0, 1);
		const bool repeat = mode == "repeat" || mode == "cancel";
		if(repeat)
		{
			const auto initial = runImport(hardware, first, mode == "cancel");
			if(initial.state != (mode == "cancel" ? md::MidiSysexTransferState::Cancelled : md::MidiSysexTransferState::Complete)) return false;
			advance(hardware, md::g_samplerate * delay);
		}
		const bool restored = mode == "restored" || mode == "uncached";
		const auto target = repeat || restored ? second : first;
		std::printf("READINESS START mode=%s t=%.3f midi=%u cache=%u flashIdle=%.3f pc=%x\n", mode.c_str(),
			double(hardware.getUC().getCycles())/40'000'000, hardware.isFirmwareMidiReady(), hardware.isFactoryFlashCacheReady(),
			double(hardware.getUC().flashIdleCycles())/40'000'000, hardware.getUC().getPC());
		const auto progress = runImport(hardware, target);
		const bool immediateContents = containsSamples(hardware.copyFlashData(), expectedSamples(target));
		std::printf("READINESS IMMEDIATE t=%.6f contents=%u\n",
			double(hardware.getUC().getCycles()) / 40'000'000, immediateContents);
		advance(hardware, md::g_samplerate * 20);
		const auto flash = hardware.copyFlashData();
		const bool contents = containsSamples(flash, expectedSamples(target));
		const bool priorPreserved = (mode != "repeat" && !restored) || containsSamples(flash, expectedSamples(first));
		std::printf("READINESS RESULT mode=%s delay=%u state=%u error=%u retries=%u contents=%u priorPreserved=%u acked=%u\n",
			mode.c_str(), delay, unsigned(progress.state), unsigned(progress.error), progress.retries,
			contents, priorPreserved, progress.acknowledgedSamples);
		return progress.state == md::MidiSysexTransferState::Complete && contents && priorPreserved;
	}

}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if(argc != 4 && argc != 5)
	{
		std::puts("usage: mdSdsFirmwareTest <MD-ROM> <sample.syx|--generated|--generated-bank|--generated-mixed|--generated-cancel> <factory.cache> [none|corrupt-packet|drop-ack|delay-ack|duplicate-ack|wait|silence|drop-header-ack|drop-final-ack|boot:seconds|repeat:seconds|cancel:seconds|probe:seconds|restored:seconds|uncached:seconds|cold:seconds|quiet:seconds|pc:0|metadata-only:seconds|observe:seconds|observe-restored:seconds]");
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
	const std::string testMode = argc == 5 ? argv[4] : "";
	// Device materializes the cache before constructing Hardware. The previous
	// harness passed only cache metadata, incorrectly pairing "initialized" flags
	// with raw, uninitialized ROM flash. Retain that setup only as a named control.
	const bool metadataOnly = testMode.find("metadata-only:") == 0;
	auto machine = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum,
		std::vector<uint8_t>{}, std::shared_ptr<md::FrontPanelPublisher>{}, metadataOnly ? std::vector<uint8_t>{} : baseline, cache);
	if(!metadataOnly && machine->copyFlashData() != baseline) return 1;
	const auto separator = testMode.find(':');
	const std::string readinessMode = testMode.substr(0, separator);
	const bool readiness = separator != std::string::npos
		&& (readinessMode == "boot" || readinessMode == "repeat" || readinessMode == "cancel" || readinessMode == "probe"
			|| readinessMode == "restored" || readinessMode == "uncached" || readinessMode == "cold"
			|| readinessMode == "quiet" || readinessMode == "pc" || readinessMode == "metadata-only"
			|| readinessMode == "observe" || readinessMode == "observe-restored");
	uint32_t delay = 20;
	if(readiness)
	{
		const auto text = testMode.substr(separator + 1);
		char* end = nullptr;
		const auto parsed = std::strtoul(text.c_str(), &end, 10);
		if(text.empty() || *end || parsed > 60) return 2;
		delay = uint32_t(parsed);
	}
	if(readiness && (readinessMode == "restored" || readinessMode == "uncached" || readinessMode == "observe-restored"))
	{
		if(!boot(*machine) || runImport(*machine, md::test::sdsSample()).state != md::MidiSysexTransferState::Complete) return 1;
		advance(*machine, md::g_samplerate * 20);
		std::vector<uint8_t> state;
		md::DecodedState decoded;
		if(!containsSamples(machine->copyFlashData(), expectedSamples(md::test::sdsSample()))
			|| !md::encodeStateWithFactoryBaseline(state, machine->copyPatchRam(), machine->copyFlashData(), baseline,
				rom, md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
			|| !md::decodeState(decoded, state, rom, md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)) return 1;
		auto flash = baseline;
		if(!md::applyFlashOverlay(flash, decoded.flashOverlay, baseline, rom)) return 1;
		machine.reset();
		machine = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum,
			decoded.patchRam, std::shared_ptr<md::FrontPanelPublisher>{}, flash,
			readinessMode == "uncached" ? std::vector<uint8_t>{} : cache);
	}
	if(readiness && readinessMode == "cold")
	{
		machine.reset();
		machine = std::make_unique<md::Hardware>(rom, argv[1]);
		if(const auto* prefix = std::getenv("MD_SDS_READINESS_TRACE"))
			readinessTrace = std::make_unique<md::test::ReadinessTrace>(std::string(prefix) + "-init");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while(!machine->isFactoryFlashReadyForReboot() && std::chrono::steady_clock::now() < deadline) step(*machine, 64);
		if(!machine->isFactoryFlashReadyForReboot()) return 1;
		const auto flash = machine->copyFlashData(), patch = machine->copyPatchRam();
		machine.reset();
		machine = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum,
			patch, std::shared_ptr<md::FrontPanelPublisher>{}, flash);
	}
	if(readiness)
		if(const auto* prefix = std::getenv("MD_SDS_READINESS_TRACE"))
			readinessTrace = std::make_unique<md::test::ReadinessTrace>(prefix);
	auto& hardware = *machine;
	if(std::getenv("MD_SDS_FLASH_TRACE"))
	{
		const auto reference = expectedSamples(md::test::sdsSample()).front();
		hardware.getUC().setFlashOperationObserver([reference, matched = size_t{0}, next = uint32_t{0}]
			(const md::FlashCommandDecoder::Operation& op, uint64_t cycles) mutable
		{
			if(op.type == md::FlashCommandDecoder::Operation::Type::EraseSector)
			{
				std::printf("NOR ERASE t=%.6f offset=%x size=%x\n", double(cycles)/40'000'000,
					md::FlashCommandDecoder::eraseSectorBegin(op.offset), md::FlashCommandDecoder::eraseSectorSize(op.offset));
				matched = 0;
				return;
			}
			if(op.offset != next) matched = 0;
			const auto word = (uint16_t(reference[matched*2]) << 8) | reference[matched*2+1];
			matched = op.value == word ? matched + 1 : 0;
			next = op.offset + 2;
			if(matched == reference.size()/2)
			{
				std::printf("NOR SAMPLE PROGRAMMED t=%.6f offset=%zx words=%zu\n", double(cycles)/40'000'000,
					size_t(op.offset + 2 - reference.size()), matched);
				matched = 0;
			}
		});
	}
	if(!hardware.isValid()
		|| !boot(hardware, readinessMode == "quiet" || readinessMode == "pc" ? 0
			: readiness && readinessMode != "repeat" && readinessMode != "cancel" ? delay : 20)) return 1;
	if(readiness) return readinessTest(hardware, readinessMode, delay) ? 0 : 1;
	if(argc == 5) return faultTest(hardware, bytes, samples, testMode, baseline, rom) ? 0 : 1;
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
