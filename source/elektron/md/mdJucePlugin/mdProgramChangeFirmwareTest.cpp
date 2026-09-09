#include "mdAutomationTestSupport.h"
#include "mdLib/mdmidiprotocol.h"

#include <algorithm>
#include <functional>
#include <iostream>

namespace
{
	using namespace mdAutomationTest;
	using Message = md::automation::sysex::Message;
	constexpr auto Model = md::MachineModel::Machinedrum;
	using Status = md::automation::sysex::StatusParameter;
	using Source = synthLib::MidiEventSource;
	constexpr auto ProgramChange = synthLib::MidiRoutingMatrix::EventType::ProgramChange;
	// Global dump v6 (MD OS 1.63): base channel and Program Change IN/OUT bits.
	constexpr size_t BaseChannelOffset = 0xad;
	constexpr size_t ProgramChangeOffset = 0xbe;

	class MidiHarness
	{
	public:
		Harness product{Model};
		juce::AudioBuffer<float> audio{2, BlockSize};
		juce::MidiBuffer midi;
		std::vector<Message> replies;

		void process(const int blocks = 1)
		{
			for(int block = 0; block < blocks; ++block)
			{
				audio.clear();
				product.audioProcessor.processBlock(audio, midi);
				for(const auto metadata : midi)
				{
					const auto message = metadata.getMessage();
					if(message.isSysEx())
						replies.emplace_back(message.getRawData(),
							message.getRawData() + message.getRawDataSize());
				}
				midi.clear();
			}
		}

		void send(const Message& message)
		{
			midi.addEvent(juce::MidiMessage(message.data(), static_cast<int>(message.size())), 0);
			process();
		}

		Message exchange(const Message& request, const std::function<bool(const Message&)>& matches)
		{
			replies.clear();
			send(request);
			for(int block = 0; block < 4000; ++block)
			{
				const auto found = std::find_if(replies.begin(), replies.end(), matches);
				if(found != replies.end()) return *found;
				process();
			}
			throw std::runtime_error("firmware MIDI reply timed out");
		}

		uint8_t status(const md::automation::sysex::StatusParameter parameter)
		{
			const auto reply = exchange(md::automation::sysex::statusRequest(Model, parameter),
				[parameter](const Message& m)
				{
					const auto parsed = md::automation::sysex::parseStatusResponse(Model, m);
					return parsed && parsed->parameter == parameter;
				});
			return md::automation::sysex::parseStatusResponse(Model, reply)->value;
		}

		Message global()
		{
			const auto slot = status(md::automation::sysex::StatusParameter::Global);
			return exchange(md::automation::sysex::globalRequest(Model, slot),
				[slot](const Message& m)
				{
					const auto parsed = md::automation::sysex::parseGlobalDump(Model, m);
					return parsed && parsed->slot == slot;
				});
		}

		void selectPattern(const uint8_t pattern)
		{
			const auto body = md::midiProtocol::selectPattern(Model, pattern);
			midi.addEvent(juce::MidiMessage::createSysExMessage(body.data(), static_cast<int>(body.size())), 0);
			process(64);
		}

		void configure(const Message& original, const uint8_t mode, const uint8_t baseChannel = 0)
		{
			auto changed = original;
			changed[BaseChannelOffset] = baseChannel;
			changed[ProgramChangeOffset] = mode;
			changed.resize(changed.size() - 5);
			finishDump(changed);
			send(changed);
			process(64);
			// Reload the stored Global so the firmware applies the new settings.
			send({0xf0, 0, 0x20, 0x3c, 2, 0, 0x71, 0x01, original[9], 0xf7});
			process(64);
			const auto stored = global();
			require(stored.size() == original.size()
				&& stored[BaseChannelOffset] == baseChannel
				&& stored[ProgramChangeOffset] == mode, "Global configuration did not round trip");
		}

		void program(const int channel, const int pattern, const Source source = Source::Host)
		{
			const auto message = juce::MidiMessage::programChange(channel, pattern);
			if(source == Source::Physical)
				product.processor.handleIncomingMidiMessage(nullptr, message);
			else
				midi.addEvent(message, 0);
			process(384);
		}

		void expect(const uint8_t pattern, const std::string& context, const bool checkKit = true)
		{
			require(status(Status::Pattern) == pattern, context + ": wrong firmware pattern");
			if(checkKit)
			{
				require(status(Status::Kit) == pattern, context + ": wrong firmware kit");
				require(product.controller.isAutomationSynchronized(), context + ": editor is not synchronized");
				require(product.controller.getParameter("Level", 0)->getUnnormalizedValue()
					== (pattern == 0 ? 35 : 93), context + ": stale editor Level");
			}
			std::cout << "PASS " << context << '\n';
		}
	};
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		MidiHarness h;
		if(!h.product.hasLocalFirmware())
			return allowMissingFirmware("mdProgramChangeFirmwareTest", Model) ? 0 : SkipReturnCode;
		h.product.prepare();
		require(h.product.synchronize(), "initial firmware synchronization failed");
		h.product.processor.getMidiRoutingMatrix().setEnabled(synthLib::MidiEventSource::Device,
			synthLib::MidiEventSource::Host, synthLib::MidiRoutingMatrix::EventType::SysEx, true);
		const auto global = h.global();
		require(global.size() == 197 && global[7] == 6,
			"fixture requires the MD OS 1.63 v6 Global layout");
		// Build two distinguishable kits through the host/controller and firmware
		// SysEx paths. Extended mode remembers each pattern's selected kit.
		require(global[0xb1] == 1, "fixture requires Extended mode");
		for(uint8_t slot = 0; slot < 2; ++slot)
		{
			h.selectPattern(slot);
			h.send({0xf0,0,0x20,0x3c,2,0,0x71,0x02,slot,0xf7});
			h.process(64);
			require(h.product.synchronize(), "kit setup synchronization failed");
			auto* parameter = h.product.controller.getParameter("Level", 0);
			require(parameter != nullptr, "Level parameter missing");
			hostWrite(*parameter, slot == 0 ? 35 : 93);
			h.process(64);
			h.send(md::automation::sysex::kitSave(Model, slot));
			h.process(64);
		}
		for(uint8_t mode = 0; mode < 4; ++mode)
		{
			h.configure(global, mode);
			h.selectPattern(0);
			require(h.status(Status::Pattern) == 0, "pattern reset failed");
			h.program(1, 1);
			h.expect(mode & 1 ? 1 : 0, "firmware Program Change mode " + std::to_string(mode));
		}
		h.configure(global, 1);
		h.program(1, 0);
		h.expect(0, "program zero returns to first pattern and kit");
		h.program(16, 1);
		h.expect(0, "wrong channel is ignored");
		h.configure(global, 1, 4); // MIDI channel 5, encoded zero-based in the dump.
		h.program(1, 1);
		h.expect(0, "old base channel is ignored");
		h.program(5, 1);
		h.expect(1, "configured base channel is honored");
		h.program(5, 127);
		h.expect(127, "last MIDI program reaches the last pattern", false);
		h.program(5, 0);
		h.expect(0, "return from last pattern");

		auto& routing = h.product.processor.getMidiRoutingMatrix();
		routing.setEnabled(Source::Host, Source::Device, ProgramChange, false);
		h.program(5, 1);
		h.expect(0, "disabled DAW route blocks Program Change");
		routing.setEnabled(Source::Host, Source::Device, ProgramChange, true);
		h.program(5, 1);
		h.expect(1, "enabled DAW route delivers Program Change");
		routing.setEnabled(Source::Physical, Source::Device, ProgramChange, false);
		h.program(5, 0, Source::Physical);
		h.expect(1, "disabled MIDI port route blocks Program Change");
		routing.setEnabled(Source::Physical, Source::Device, ProgramChange, true);
		h.program(5, 0, Source::Physical);
		h.expect(0, "MIDI input callback delivers Program Change");

		std::cout << "mdProgramChangeFirmwareTest: pattern/kit selection, editor synchronization, "
			"firmware settings, channel filtering, and routing passed\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdProgramChangeFirmwareTest: " << error.what() << '\n';
		return 1;
	}
}
