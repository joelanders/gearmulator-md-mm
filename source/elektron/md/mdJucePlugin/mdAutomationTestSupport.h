#pragma once

#include "mdController.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdautomation.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdsysexautomation.h"

#include "juce_events/juce_events.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <sstream>
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

	inline bool firmwareTestsRequired()
	{
		const auto* const value = std::getenv("MD_AUTOMATION_REQUIRE_FIRMWARE");
		return value != nullptr && std::string(value) == "1";
	}

	inline bool allowMissingFirmware(const char* const _suite,
		const md::MachineModel _model)
	{
		if(firmwareTestsRequired())
			throw std::runtime_error(std::string(_suite) + ": required "
				+ modelName(_model) + " firmware fixture is unavailable");
		std::cout << _suite << ": SKIP " << modelName(_model)
			<< " (firmware unavailable)\n";
		return true;
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
			audioProcessor.setNonRealtime(true);
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
			}
		}

		bool synchronize(const int _maximumBlocks = 12000)
		{
			for(int block = 0; block < _maximumBlocks; ++block)
			{
				process(1);
				if(controller.isAutomationSynchronized())
					return true;
			}
			return false;
		}

		std::string firmwareReadiness()
		{
			std::ostringstream result;
			processor.getPlugin().withDeviceLocked(
				[this, &result](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					if(device == nullptr)
					{
						result << "device=nonlocal";
						return;
					}
					auto& hardware = device->getHardware();
					const auto panel = hardware.getFrontPanelSnapshot();
					result << "audio=" << hardware.isAudioReady()
						<< ", mixer=" << hardware.getDspMixer().booted()
						<< ", producer=" << hardware.getDspProducer().booted()
						<< ", pixels=" << panel.countLitPixels()
						<< ", tiles=" << panel.getTileWriteCount()
						<< ", midiQueued=" << hardware.queuedMidiRxBytes()
						<< ", midiConsumed=" << hardware.midiRxConsumedCount()
						<< ", midiOverflows=" << hardware.midiRxOverflowCount()
						<< ", ingressContention="
						<< controller.getRealtimeMidiIngressContentionDropCount()
						<< ", ingressCapacity="
						<< controller.getRealtimeMidiIngressCapacityDropCount();
					result
						<< ", ucCycles=" << hardware.getUC().getCycles()
						<< ", ucPC=" << hardware.getUC().getPC();
				});
			return result.str();
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

	inline bool firmwareMidiReady(Harness& _harness)
	{
		bool ready = false;
		_harness.processor.getPlugin().withDeviceLocked(
			[&ready](synthLib::Device* const _device)
			{
				if(const auto* const device = dynamic_cast<md::Device*>(_device))
					ready = device->getHardware().isFirmwareMidiReady();
			});
		return ready;
	}

	inline void hostWrite(pluginLib::Parameter& _parameter, const int _value)
	{
		_parameter.setValue(_parameter.getNormalisableRange().convertTo0to1(
			static_cast<float>(_value)));
	}

	inline void finishDump(md::automation::sysex::Message& _message)
	{
		uint32_t checksum = 0;
		for(size_t index = 9; index < _message.size(); ++index)
			checksum += _message[index];
		checksum &= 0x3fff;
		const auto length = static_cast<uint16_t>(_message.size() - 5);
		_message.push_back(static_cast<uint8_t>(checksum >> 7));
		_message.push_back(static_cast<uint8_t>(checksum & 0x7f));
		_message.push_back(static_cast<uint8_t>(length >> 7));
		_message.push_back(static_cast<uint8_t>(length & 0x7f));
		_message.push_back(0xf7);
	}

	inline std::vector<uint8_t> packMmPayload(const std::vector<uint8_t>& _decoded)
	{
		std::vector<uint8_t> rle;
		for(size_t position = 0; position < _decoded.size();)
		{
			const auto value = _decoded[position];
			size_t count = 1;
			while(position + count < _decoded.size()
				&& _decoded[position + count] == value && count < 127)
				++count;
			if(value >= 0x80 || count > 1)
			{
				rle.push_back(static_cast<uint8_t>(0x80 | count));
				rle.push_back(value);
			}
			else
				rle.push_back(value);
			position += count;
		}

		std::vector<uint8_t> packed;
		for(size_t position = 0; position < rle.size();)
		{
			const auto header = packed.size();
			packed.push_back(0);
			for(uint8_t bit = 0; bit < 7 && position < rle.size(); ++bit)
			{
				const auto value = rle[position++];
				if(value & 0x80)
					packed[header] |= static_cast<uint8_t>(1u << (6u - bit));
				packed.push_back(static_cast<uint8_t>(value & 0x7f));
			}
		}
		return packed;
	}

	inline pluginLib::SysEx makeDump(const md::MachineModel _model,
		const uint8_t _command, const uint8_t _slot,
		const std::vector<uint8_t>& _decoded)
	{
		md::automation::sysex::Message result{
			0xf0, 0x00, 0x20, 0x3c,
			static_cast<uint8_t>(_model == md::MachineModel::Monomachine ? 0x03 : 0x02),
			0x00, _command, 0x01, 0x01, _slot};
		if(_model == md::MachineModel::Monomachine)
		{
			const auto packed = packMmPayload(_decoded);
			result.insert(result.end(), packed.begin(), packed.end());
		}
		else
			result.insert(result.end(), _decoded.begin(), _decoded.end());
		finishDump(result);
		return {result.begin(), result.end()};
	}

	inline pluginLib::SysEx makeGlobalDump(
		const md::MachineModel _model, const uint8_t _slot, const uint8_t _base)
	{
		if(_model == md::MachineModel::Monomachine)
		{
			std::vector<uint8_t> decoded(260, 0);
			decoded[0] = 0x91;
			decoded[1] = _base;
			return makeDump(_model, 0x50, _slot, decoded);
		}
		// makeDump contributes the ten-byte header before this raw payload, so the
		// firmware's absolute base-channel offset 0xad maps to payload offset 0xa3.
		std::vector<uint8_t> decoded(0xa6, 0);
		decoded[0xa3] = _base;
		return makeDump(_model, 0x50, _slot, decoded);
	}

	inline pluginLib::SysEx makeKitDump(
		const md::MachineModel _model, const uint8_t _slot, const uint8_t _value)
	{
		if(_model == md::MachineModel::Monomachine)
		{
			std::vector<uint8_t> decoded(698, 0);
			for(uint8_t track = 0; track < md::automation::monomachine::TrackCount;
				++track)
			{
				decoded[0x0b + track] = _value;
				for(uint8_t parameter = 0; parameter < 56; ++parameter)
					decoded[0x11 + track * 72 + parameter] = _value;
			}
			return makeDump(_model, 0x52, _slot, decoded);
		}
		std::vector<uint8_t> decoded(1218, 0);
		for(uint8_t track = 0; track < md::automation::machinedrum::TrackCount;
			++track)
		{
			for(uint8_t parameter = 0; parameter < 24; ++parameter)
				decoded[0x10 + track * 24 + parameter] = _value;
			decoded[0x190 + track] = _value;
		}
		return makeDump(_model, 0x52, _slot, decoded);
	}
}
