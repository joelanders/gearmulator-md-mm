#include "mdController.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdautomation.h"
#include "mdLib/mddevice.h"
#include "juce_events/juce_events.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
	void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		throw std::runtime_error(_message);
	}

	void process(mdJucePlugin::AudioPluginAudioProcessor& _processor,
		const int _blocks)
	{
		juce::AudioBuffer<float> audio(2, 128);
		juce::MidiBuffer midi;
		auto& audioProcessor = static_cast<juce::AudioProcessor&>(_processor);
		for(int block = 0; block < _blocks; ++block)
		{
			audio.clear();
			midi.clear();
			audioProcessor.processBlock(audio, midi);
			_processor.getController().processPendingMidiMessages();
		}
	}

	struct MidiTelemetry
	{
		uint64_t consumed = 0;
		size_t overflows = 0;
	};

	MidiTelemetry midiTelemetry(
		mdJucePlugin::AudioPluginAudioProcessor& _processor)
	{
		MidiTelemetry result;
		_processor.getPlugin().withDeviceLocked(
			[&result](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
				{
					result.consumed = device->getHardware().midiRxConsumedCount();
					result.overflows = device->getHardware().midiRxOverflowCount();
				}
			});
		return result;
	}

	bool waitForSynchronization(mdJucePlugin::AudioPluginAudioProcessor& _processor,
		mdJucePlugin::Controller& _controller)
	{
		for(int attempt = 0; attempt < 2500; ++attempt)
		{
			process(_processor, 1);
			if(_controller.isAutomationSynchronized())
				return true;
		}
		return false;
	}

	bool hasLocalFirmware(mdJucePlugin::AudioPluginAudioProcessor& _processor)
	{
		return _processor.getPlugin().withDeviceLocked(
			[](synthLib::Device* const _device)
			{
				return dynamic_cast<md::Device*>(_device) != nullptr;
			});
	}

	pluginLib::Parameter& parameter(mdJucePlugin::Controller& _controller,
		const md::MachineModel)
	{
		auto* const result = _controller.getParameter("Level", 0);
		require(result != nullptr, "test parameter was not registered");
		return *result;
	}

	void hostWrite(pluginLib::Parameter& _parameter, const int _value)
	{
		_parameter.setValue(_parameter.getNormalisableRange().convertTo0to1(
			static_cast<float>(_value)));
	}

	void verifyModel(const md::MachineModel _model)
	{
		mdJucePlugin::AudioPluginAudioProcessor processor(_model,
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{}, false);
		auto& audioProcessor = static_cast<juce::AudioProcessor&>(processor);
		audioProcessor.prepareToPlay(48000.0, 128);
		if(!hasLocalFirmware(processor))
		{
			std::cout << "mdAutomationFirmwareTest: SKIP "
				<< (_model == md::MachineModel::Monomachine ? "MM" : "MD")
				<< " (firmware unavailable)\n";
			return;
		}

		auto& controller = dynamic_cast<mdJucePlugin::Controller&>(
			processor.getController());
		require(waitForSynchronization(processor, controller),
			"initial firmware synchronization timed out (global "
			+ std::to_string(controller.hasAutomationGlobalSnapshot()) + ", kit "
			+ std::to_string(controller.hasAutomationKitSnapshot()) + ", base "
			+ std::to_string(controller.getAutomationBaseChannel()) + ")");
		require(controller.getAutomationBaseChannel() < 16,
			"firmware Global has MIDI base channel set to NONE");

		auto& probe = parameter(controller, _model);
		const auto beforeTransmitCount =
			controller.getTransmittedAutomationChangeCount();

		// Make the cache say zero without touching firmware, then require an explicit
		// repeated/default host write to reach and be consumed by the firmware UART.
		probe.setValueFromSynth(0, pluginLib::Parameter::Origin::PresetChange);
		const auto beforeZero = midiTelemetry(processor);
		hostWrite(probe, 0);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeTransmitCount + 1,
			"host write did not produce exactly one automation CC");
		process(processor, 32);
		const auto afterZero = midiTelemetry(processor);
		require(afterZero.consumed >= beforeZero.consumed + 3
			&& afterZero.overflows == beforeZero.overflows,
			"explicit zero automation CC was not consumed losslessly by firmware");

		const auto beforeChanged = midiTelemetry(processor);
		hostWrite(probe, 127);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeTransmitCount + 2,
			"changed host write did not produce exactly one automation CC");
		process(processor, 32);
		const auto afterChanged = midiTelemetry(processor);
		require(afterChanged.consumed >= beforeChanged.consumed + 3
			&& afterChanged.overflows == beforeChanged.overflows,
			"changed automation CC was not consumed losslessly by firmware");

		const auto beforeStressCount =
			controller.getTransmittedAutomationChangeCount();
		const auto beforeStressMidi = midiTelemetry(processor);
		size_t expectedWrites = 0;
		int ordinal = 0;
		for(auto* const audioParameter : audioProcessor.getParameters())
		{
			auto* const automationParameter = dynamic_cast<pluginLib::Parameter*>(
				audioParameter);
			if(!automationParameter)
				continue;
			const auto& description = automationParameter->getDescription();
			const auto mutePage = _model == md::MachineModel::Monomachine
				? md::automation::monomachine::Mute
				: md::automation::machinedrum::Mute;
			if(description.page == mutePage)
				continue;
			const auto value = (ordinal * 37 + 19) & 0x7f;
			hostWrite(*automationParameter, value);
			++expectedWrites;
			if((++ordinal & 31) == 0)
				process(processor, 4);
		}
		process(processor, 64);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeStressCount + expectedWrites,
			"automation stress pass did not transmit every host write");
		const auto afterStressMidi = midiTelemetry(processor);
		require(afterStressMidi.consumed
			>= beforeStressMidi.consumed + expectedWrites * 3
			&& afterStressMidi.overflows == beforeStressMidi.overflows,
			"automation stress stream was not consumed losslessly by firmware");

		hostWrite(probe, 47);
		process(processor, 32);
		juce::MemoryBlock savedState;
		audioProcessor.getStateInformation(savedState);
		require(!savedState.isEmpty(), "processor produced empty persisted state");
		hostWrite(probe, 12);
		process(processor, 32);
		require(probe.getUnnormalizedValue() == 12,
			"pre-restore control write failed");
		audioProcessor.setStateInformation(savedState.getData(),
			static_cast<int>(savedState.getSize()));
		const auto beforeRestoreFlush =
			controller.getTransmittedAutomationChangeCount();
		require(waitForSynchronization(processor, controller),
			"state restore did not resynchronize automation");
		require(probe.getUnnormalizedValue() == 47,
			"firmware-backed automation value was not restored from DAW state");
		require(controller.getTransmittedAutomationChangeCount()
			>= beforeRestoreFlush + audioProcessor.getParameters().size(),
			"DAW-state automation snapshot was not flushed back to firmware");

		audioProcessor.releaseResources();
		std::cout << "mdAutomationFirmwareTest: "
			<< (_model == md::MachineModel::Monomachine ? "MM" : "MD")
			<< " PASS\n";
	}
}

int main(const int _argc, const char* const* _argv)
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		const auto only = _argc > 1 ? std::string(_argv[1]) : std::string();
		if(only != "--mm")
			verifyModel(md::MachineModel::Machinedrum);
		if(only != "--md")
			verifyModel(md::MachineModel::Monomachine);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdAutomationFirmwareTest: " << error.what() << '\n';
		return 1;
	}
}
