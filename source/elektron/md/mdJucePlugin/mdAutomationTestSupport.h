#pragma once

#include "mdController.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdautomation.h"
#include "mdLib/mddevice.h"

#include "juce_events/juce_events.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdAutomationTest
{
	constexpr int BlockSize = 128;

	inline void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		throw std::runtime_error(_message);
	}

	inline const char* modelName(const md::MachineModel _model)
	{
		return _model == md::MachineModel::Monomachine ? "MM" : "MD";
	}

	struct MidiTelemetry
	{
		uint64_t consumed = 0;
		size_t overflows = 0;
		uint64_t contentionDrops = 0;
		uint64_t capacityDrops = 0;
	};

	class StoppedPlayHead final : public juce::AudioPlayHead
	{
	public:
		juce::Optional<PositionInfo> getPosition() const override
		{
			PositionInfo result;
			result.setIsPlaying(false);
			result.setIsRecording(false);
			result.setBpm(120.0);
			result.setPpqPosition(0.0);
			return result;
		}
	};

	class Harness
	{
	public:
		explicit Harness(const md::MachineModel _model)
			: model(_model)
			, processor(_model,
				mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{}, false)
			, audioProcessor(static_cast<juce::AudioProcessor&>(processor))
			, controller(dynamic_cast<mdJucePlugin::Controller&>(
				processor.getController()))
			, audio(2, BlockSize)
		{
			audioProcessor.setPlayHead(&playHead);
		}

		~Harness()
		{
			if(prepared)
				audioProcessor.releaseResources();
		}

		bool hasLocalFirmware()
		{
			return processor.getPlugin().withDeviceLocked(
				[](synthLib::Device* const _device)
				{
					return dynamic_cast<md::Device*>(_device) != nullptr;
				});
		}

		void prepare()
		{
			if(prepared)
				return;
			audioProcessor.prepareToPlay(48000.0, BlockSize);
			prepared = true;
		}

		void process(const int _blocks)
		{
			require(prepared, "attempted to process an unprepared instance");
			for(int block = 0; block < _blocks; ++block)
			{
				audio.clear();
				midi.clear();
				audioProcessor.processBlock(audio, midi);
				controller.processPendingMidiMessages();
			}
		}

		bool synchronize(const int _maximumBlocks = 3000)
		{
			for(int block = 0; block < _maximumBlocks; ++block)
			{
				process(1);
				if(controller.isAutomationSynchronized())
					return true;
			}
			return false;
		}

		MidiTelemetry telemetry()
		{
			MidiTelemetry result;
			result.contentionDrops =
				controller.getRealtimeMidiIngressContentionDropCount();
			result.capacityDrops =
				controller.getRealtimeMidiIngressCapacityDropCount();
			processor.getPlugin().withDeviceLocked(
				[&result](synthLib::Device* const _device)
				{
					if(const auto* const device = dynamic_cast<md::Device*>(_device))
					{
						result.consumed =
							device->getHardware().midiRxConsumedCount();
						result.overflows =
							device->getHardware().midiRxOverflowCount();
					}
				});
			return result;
		}

		const md::MachineModel model;
		StoppedPlayHead playHead;
		mdJucePlugin::AudioPluginAudioProcessor processor;
		juce::AudioProcessor& audioProcessor;
		mdJucePlugin::Controller& controller;

	private:
		juce::AudioBuffer<float> audio;
		juce::MidiBuffer midi;
		bool prepared = false;
	};

	inline std::vector<pluginLib::Parameter*> parameters(Harness& _harness,
		const bool _includeMutes = true)
	{
		std::vector<pluginLib::Parameter*> result;
		const auto mutePage = _harness.model == md::MachineModel::Monomachine
			? md::automation::monomachine::Mute
			: md::automation::machinedrum::Mute;
		for(auto* const audioParameter : _harness.audioProcessor.getParameters())
		{
			auto* const parameter =
				dynamic_cast<pluginLib::Parameter*>(audioParameter);
			if(parameter && (_includeMutes
				|| parameter->getDescription().page != mutePage))
				result.push_back(parameter);
		}
		return result;
	}

	inline void hostWrite(pluginLib::Parameter& _parameter, const int _value)
	{
		_parameter.setValue(_parameter.getNormalisableRange().convertTo0to1(
			static_cast<float>(_value)));
	}
}
