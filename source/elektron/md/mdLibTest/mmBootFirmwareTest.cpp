#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdsysexautomation.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
	constexpr auto g_model = md::MachineModel::Monomachine;
	namespace sysex = md::automation::sysex;

	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		for(uint32_t done = 0; done < _frames;)
		{
			const auto chunk = std::min<uint32_t>(256, _frames - done);
			_hardware.advance(chunk);
			done += chunk;
		}
	}

	bool sameLcd(const md::FrontPanel& _a, const md::FrontPanel& _b)
	{
		for(uint32_t y = 0; y < md::FrontPanel::g_lcdHeight; ++y)
			for(uint32_t x = 0; x < md::FrontPanel::g_lcdWidth; ++x)
				if(_a.getLcdPixel(x, y) != _b.getLcdPixel(x, y))
					return false;
		return true;
	}

	void tap(md::Hardware& _hardware, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(g_model, _control);
		require(packet.has_value(), "missing MM panel mapping");
		require(_hardware.trySendPanelEvent(packet->row, packet->mask), "panel press rejected");
		advance(_hardware, 2048);
		require(_hardware.trySendPanelEvent(packet->row, 0), "panel release rejected");
		advance(_hardware, md::g_samplerate / 2);
	}

	void testPanelProgress(md::Hardware& _hardware)
	{
		// Exercise the UART panel path after boot, without inspecting firmware task
		// memory. Repeated menu entry/exit must keep producing fresh LCD content.
		for(uint32_t iteration = 0; iteration < 3; ++iteration)
		{
			const auto before = _hardware.getFrontPanelSnapshot();
			tap(_hardware, md::PanelControl::Tempo);
			const auto tempo = _hardware.getFrontPanelSnapshot();
			require(!sameLcd(before, tempo), "MM tempo menu did not update LCD");
			tap(_hardware, md::PanelControl::Exit);
			require(!sameLcd(tempo, _hardware.getFrontPanelSnapshot()),
				"MM menu exit did not update LCD");
		}
	}

	void send(md::Hardware& _hardware, const sysex::Message& _bytes)
	{
		synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
		event.sysex.assign(_bytes.begin(), _bytes.end());
		require(_hardware.sendMidi(event), "MM rejected SysEx message");
	}

	template<typename Parser>
	auto query(md::Hardware& _hardware, const sysex::Message& _bytes,
		const Parser& _parse, const char* _failure)
	{
		std::vector<synthLib::SMidiEvent> events;
		_hardware.readMidiOut(events);
		send(_hardware, _bytes);
		for(unsigned attempt = 0; attempt < 40; ++attempt)
		{
			advance(_hardware, md::g_samplerate / 10);
			events.clear();
			_hardware.readMidiOut(events);
			for(const auto& event : events)
				if(const auto response = _parse(event.sysex)) return *response;
		}
		throw std::runtime_error(_failure);
	}

	uint8_t status(md::Hardware& _hardware, const sysex::StatusParameter _parameter)
	{
		return query(_hardware, sysex::statusRequest(g_model, _parameter),
			[&](const auto& _bytes)
			{
				auto response = sysex::parseStatusResponse(g_model, _bytes);
				if(response && response->parameter != _parameter) response.reset();
				return response;
			}, "MM firmware did not answer status request").value;
	}

	void boot(md::Hardware& _hardware)
	{
		advance(_hardware, md::g_samplerate * 20);
		require(_hardware.isAudioReady(), "MM DSPs did not finish boot");
		require(_hardware.isFirmwareMidiReady(), "MM panel/MIDI startup did not complete");
		testPanelProgress(_hardware);
	}

	std::vector<md::automation::ParameterChange> editKit(md::Hardware& _hardware,
		const uint8_t _slot, const uint8_t _baseChannel, const uint8_t _value)
	{
		// Public MIDI CC mapping: exercise two parameter pages on all six tracks,
		// then save through firmware. No firmware-private RAM edits or inspections.
		std::vector<md::automation::ParameterChange> changes;
		for(uint8_t track = 0; track < md::automation::monomachine::TrackCount; ++track)
			for(const auto page : {md::automation::monomachine::Level, md::automation::monomachine::Filter})
			{
				const md::automation::ParameterChange change{page, track, 0, static_cast<uint8_t>(_value + track)};
				const auto cc = md::automation::encodeParameterChange(g_model, change, _baseChannel);
				require(cc.has_value(), "MM MIDI channel/parameter unavailable");
				require(_hardware.sendMidi({synthLib::MidiEventSource::Host, (*cc)[0], (*cc)[1], (*cc)[2]}),
					"MM rejected parameter CC");
				changes.push_back(change);
			}
		advance(_hardware, md::g_samplerate / 2);
		send(_hardware, sysex::kitSave(g_model, _slot));
		advance(_hardware, md::g_samplerate / 2);
		return changes;
	}

	void checkKit(md::Hardware& _hardware, const uint8_t _slot,
		const std::vector<md::automation::ParameterChange>& _expected)
	{
		const auto kit = query(_hardware, sysex::kitRequest(g_model, _slot),
			[&](const auto& _bytes)
			{
				auto result = sysex::parseKitDump(g_model, _bytes);
				if(result && result->slot != _slot) result.reset();
				return result;
			}, "MM firmware did not return valid saved kit");
		for(const auto& parameter : _expected)
			require(std::find(kit.parameters.begin(), kit.parameters.end(), parameter) != kit.parameters.end(),
				"MM saved kit lost a parameter edit");
		require(_hardware.queuedMidiRxBytes() == 0 && _hardware.midiRxOverflowCount() == 0,
			"MM MIDI input did not drain cleanly");
	}

	void checkAudio(md::Hardware& _hardware, const uint8_t _channel)
	{
		require(_hardware.sendMidi({synthLib::MidiEventSource::Host,
			static_cast<uint8_t>(0x90 | _channel), 60, 100}), "MM rejected note-on");
		std::array<int32_t, 2> minimum{0x7fffff, 0x7fffff};
		std::array<int32_t, 2> maximum{-0x800000, -0x800000};
		for(unsigned block = 0; block < 64; ++block)
		{
			_hardware.processAudio(256, 0);
			if(block < 8) continue;
			for(unsigned channel = 0; channel < 2; ++channel)
				for(unsigned frame = 0; frame < 256; ++frame)
				{
					int32_t sample = _hardware.getAudioOutputs()[channel][frame] & 0xffffff;
					if(sample & 0x800000) sample -= 0x1000000;
					minimum[channel] = std::min(minimum[channel], sample);
					maximum[channel] = std::max(maximum[channel], sample);
				}
		}
		require(_hardware.sendMidi({synthLib::MidiEventSource::Host,
			static_cast<uint8_t>(0x80 | _channel), 60, 0}), "MM rejected note-off");
		advance(_hardware, md::g_samplerate / 2);
		// A coarse boot liveness oracle, not a waveform-fidelity assertion.
		require(maximum[0] - minimum[0] > 256 || maximum[1] - minimum[1] > 256,
			"MM main audio stayed silent or constant");
	}
}

int main()
{
	const auto* path = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path || !*path)
	{
		std::cout << "mmBootFirmwareTest: SKIP (MM firmware not supplied)\n";
		return 77;
	}
	try
	{
		std::vector<uint8_t> rom;
		require(baseLib::filesystem::readFile(rom, path), "could not read MM fixture");
		require(md::RomLoader::isRomForModel(rom, g_model), "MM fixture fingerprint mismatch");
		synthLib::DeviceCreateParams params;
		params.romData = std::move(rom);
		params.romName = path;
		params.customData = md::deviceCustomData(g_model);
		// Empty homePath prevents loading any user/factory NVRAM from disk.
		auto machine = std::make_unique<md::Device>(params);
		auto& hardware = machine->getHardware();
		boot(hardware);
		const auto kit = status(hardware, sysex::StatusParameter::Kit);
		const auto global = status(hardware, sysex::StatusParameter::Global);
		const auto baseChannel = query(hardware, sysex::globalRequest(g_model, global),
			[&](const auto& _bytes)
			{
				auto result = sysex::parseGlobalDump(g_model, _bytes);
				if(result && result->slot != global) result.reset();
				return result;
			}, "MM firmware did not return global MIDI settings").baseChannel;
		const auto saved = editKit(hardware, kit, baseChannel, 25);
		checkKit(hardware, kit, saved);
		checkAudio(hardware, baseChannel);
		std::vector<uint8_t> state;
		require(machine->getState(state, synthLib::StateTypeGlobal) && !state.empty(), "MM state capture failed");
		checkKit(hardware, kit, editKit(hardware, kit, baseChannel, 75));
		const auto epoch = machine->hardwareEpoch();
		require(machine->setState(state, synthLib::StateTypeGlobal), "MM state restoration failed");
		require(machine->hardwareEpoch() != epoch, "MM state restore did not replace hardware");
		// setState replaces Hardware: do not retain the pre-reset reference.
		auto& restored = machine->getHardware();
		boot(restored);
		require(status(restored, sysex::StatusParameter::Kit) == kit, "MM restored wrong kit");
		checkKit(restored, kit, saved);
		checkAudio(restored, baseChannel);
		checkKit(restored, kit, editKit(restored, kit, baseChannel, 45));
		testPanelProgress(restored);
		std::cout << "mmBootFirmwareTest: PASS, cold boot, edited kit and state-restored boot\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mmBootFirmwareTest: " << error.what() << '\n';
		return 1;
	}
}
