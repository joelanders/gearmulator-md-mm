#include "mdAutomationTestSupport.h"

#include <iostream>
#include <string>

namespace
{
	using namespace mdAutomationTest;

	pluginLib::Parameter& parameter(mdJucePlugin::Controller& _controller)
	{
		auto* const result = _controller.getParameter("Level", 0);
		require(result != nullptr, "test parameter was not registered");
		return *result;
	}

	void verifyModel(const md::MachineModel _model)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
		{
			std::cout << "mdAutomationFirmwareTest: SKIP "
				<< modelName(_model)
				<< " (firmware unavailable)\n";
			return;
		}
		harness.prepare();

		auto& audioProcessor = harness.audioProcessor;
		auto& controller = harness.controller;
		require(harness.synchronize(),
			"initial firmware synchronization timed out (global "
			+ std::to_string(controller.hasAutomationGlobalSnapshot()) + ", kit "
			+ std::to_string(controller.hasAutomationKitSnapshot()) + ", base "
			+ std::to_string(controller.getAutomationBaseChannel()) + ", requests="
			+ std::to_string(controller.getSynchronizationRequestCount()) + ", "
			+ harness.firmwareReadiness() + ")");
		require(controller.getAutomationBaseChannel() < 16,
			"firmware Global has MIDI base channel set to NONE");

		auto& probe = parameter(controller);
		const auto beforeTransmitCount =
			controller.getTransmittedAutomationChangeCount();

		// Make the cache say zero without touching firmware, then require an explicit
		// repeated/default host write to reach and be consumed by the firmware UART.
		probe.setValueFromSynth(0, pluginLib::Parameter::Origin::PresetChange);
		const auto beforeZero = harness.telemetry();
		hostWrite(probe, 0);
		harness.process(32);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeTransmitCount + 1,
			"host write did not produce exactly one automation CC");
		const auto afterZero = harness.telemetry();
		require(afterZero.consumed >= beforeZero.consumed + 3
			&& afterZero.overflows == beforeZero.overflows,
			"explicit zero automation CC was not consumed losslessly by firmware");

		const auto beforeChanged = harness.telemetry();
		hostWrite(probe, 127);
		harness.process(32);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeTransmitCount + 2,
			"changed host write did not produce exactly one automation CC");
		const auto afterChanged = harness.telemetry();
		require(afterChanged.consumed >= beforeChanged.consumed + 3
			&& afterChanged.overflows == beforeChanged.overflows,
			"changed automation CC was not consumed losslessly by firmware");

		const auto beforeStressCount =
			controller.getTransmittedAutomationChangeCount();
		const auto beforeStressMidi = harness.telemetry();
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
				harness.process(4);
		}
		harness.process(64);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeStressCount + expectedWrites,
			"automation stress pass did not transmit every host write");
		const auto afterStressMidi = harness.telemetry();
		require(afterStressMidi.consumed
			>= beforeStressMidi.consumed + expectedWrites * 3
			&& afterStressMidi.overflows == beforeStressMidi.overflows,
			"automation stress stream was not consumed losslessly by firmware");

		hostWrite(probe, 47);
		harness.process(32);
		juce::MemoryBlock savedState;
		audioProcessor.getStateInformation(savedState);
		require(!savedState.isEmpty(), "processor produced empty persisted state");
		hostWrite(probe, 12);
		harness.process(32);
		require(probe.getUnnormalizedValue() == 12,
			"pre-restore control write failed");
		audioProcessor.setStateInformation(savedState.getData(),
			static_cast<int>(savedState.getSize()));
		const auto beforeRestoreFlush =
			controller.getTransmittedAutomationChangeCount();
		require(harness.synchronize(),
			"state restore did not resynchronize automation");
		require(probe.getUnnormalizedValue() == 47,
			"firmware-backed automation value was not restored from DAW state");
		// Synchronization can complete from inside the audio callback while its
		// realtime drain guard is held. The message-thread completion path must not
		// wait on that guard; the next bounded callback performs the replay instead.
		harness.process(32);
		const auto restoredWrites = controller.getTransmittedAutomationChangeCount()
			- beforeRestoreFlush;
		require(restoredWrites >= audioProcessor.getParameters().size(),
			"DAW-state automation snapshot flushed "
			+ std::to_string(restoredWrites) + " of "
			+ std::to_string(audioProcessor.getParameters().size())
			+ " values back to firmware");

		std::cout << "mdAutomationFirmwareTest: "
			<< modelName(_model)
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
