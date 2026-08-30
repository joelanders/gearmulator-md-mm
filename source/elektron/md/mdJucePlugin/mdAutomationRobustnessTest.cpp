#include "mdController.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdautomation.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdsysexautomation.h"
#include "synthLib/midiTypes.h"

#include "juce_events/juce_events.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	constexpr int g_blockSize = 128;

	void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		throw std::runtime_error(_message);
	}

	const char* modelName(const md::MachineModel _model)
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
			, audio(2, g_blockSize)
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
			audioProcessor.prepareToPlay(48000.0, g_blockSize);
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
						result.consumed = device->getHardware().midiRxConsumedCount();
						result.overflows = device->getHardware().midiRxOverflowCount();
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

	std::vector<pluginLib::Parameter*> parameters(Harness& _harness,
		const bool _includeMutes = true)
	{
		std::vector<pluginLib::Parameter*> result;
		const auto mutePage = _harness.model == md::MachineModel::Monomachine
			? md::automation::monomachine::Mute
			: md::automation::machinedrum::Mute;
		for(auto* const audioParameter : _harness.audioProcessor.getParameters())
		{
			auto* const parameter = dynamic_cast<pluginLib::Parameter*>(audioParameter);
			if(parameter && (_includeMutes
				|| parameter->getDescription().page != mutePage))
				result.push_back(parameter);
		}
		return result;
	}

	void hostWrite(pluginLib::Parameter& _parameter, const int _value)
	{
		_parameter.setValue(_parameter.getNormalisableRange().convertTo0to1(
			static_cast<float>(_value)));
	}

	uint64_t appendDigest(uint64_t _digest,
		const md::automation::ControlChange& _message)
	{
		for(const auto byte : _message)
		{
			_digest ^= byte;
			_digest *= 1099511628211ull;
		}
		return _digest;
	}

	class ParameterListener final : public juce::AudioProcessorParameter::Listener
	{
	public:
		void parameterValueChanged(int, const float _newValue) override
		{
			lastValue = _newValue;
			++valueChanges;
		}

		void parameterGestureChanged(int, bool) override {}

		int valueChanges = 0;
		float lastValue = -1.0f;
	};

	class ProcessorListener final : public juce::AudioProcessorListener
	{
	public:
		void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
		void audioProcessorChanged(juce::AudioProcessor*,
			const ChangeDetails& _details) override
		{
			if(_details.programChanged)
				++programChanges;
		}
		int programChanges = 0;
	};

	void verifyQueuedPreBootWrites(Harness& _harness)
	{
		auto allParameters = parameters(_harness, false);
		require(allParameters.size() >= 32, "too few automation parameters");
		std::mt19937 random(0x4d444157u + static_cast<unsigned>(_harness.model));
		std::map<pluginLib::Parameter*, int> expected;
		for(int write = 0; write < 512; ++write)
		{
			auto* const parameter = allParameters[random() % 32];
			const auto value = static_cast<int>(random() & 0x7f);
			hostWrite(*parameter, value);
			expected[parameter] = value;
		}
		require(_harness.controller.getTransmittedAutomationChangeCount() == 0,
			"automation escaped before firmware synchronization");

		_harness.prepare();
		require(_harness.synchronize(), "pre-boot write synchronization timed out");
		require(_harness.controller.getTransmittedAutomationChangeCount()
			== expected.size(), "pre-boot queue did not coalesce by address");
		for(const auto& [parameter, value] : expected)
			require(parameter->getUnnormalizedValue() == value,
				"pre-boot queue lost its newest value");
		_harness.process(96);
		require(_harness.telemetry().overflows == 0,
			"pre-boot queue overflowed firmware MIDI RX");
	}

	void verifyExternalNotifications(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& probe = *parameters(_harness, false).front();
		const auto& description = probe.getDescription();
		const auto oldValue = probe.getUnnormalizedValue();
		const auto newValue = (oldValue + 53) & 0x7f;
		const md::automation::ParameterChange change{
			description.page, probe.getPart(), description.index,
			static_cast<uint8_t>(newValue)};
		const auto encoded = md::automation::encodeParameterChange(_harness.model,
			change, controller.getAutomationBaseChannel());
		require(encoded.has_value(), "external notification probe did not encode");

		ParameterListener parameterListener;
		probe.addListener(&parameterListener);
		const synthLib::SMidiEvent event(synthLib::MidiEventSource::Physical,
			(*encoded)[0], (*encoded)[1], (*encoded)[2]);
		require(controller.parseControllerMessage(event),
			"external CC was not recognized");
		require(probe.getUnnormalizedValue() == newValue,
			"external CC did not update the DAW parameter cache");
		require(probe.getChangeOrigin() == pluginLib::Parameter::Origin::Midi,
			"external CC was assigned the wrong origin");
		require(parameterListener.valueChanges == 1,
			"external changed CC did not notify the host exactly once");
		require(std::abs(parameterListener.lastValue - probe.getValue()) < 0.0001f,
			"external CC host notification carried the wrong value");
		require(controller.parseControllerMessage(event),
			"repeated external CC was not recognized");
		require(parameterListener.valueChanges == 1,
			"repeated unchanged external CC redundantly notified the host");
		probe.removeListener(&parameterListener);

		const synthLib::SMidiEvent unmapped(synthLib::MidiEventSource::Physical,
			(*encoded)[0], 0x7f, 1);
		require(!controller.parseControllerMessage(unmapped),
			"unmapped external CC was incorrectly consumed");

		ProcessorListener processorListener;
		_harness.audioProcessor.addListener(&processorListener);
		const auto snapshot = controller.createAutomationSnapshot();
		require(snapshot.size() >= 6, "could not observe current Kit for refresh test");
		const auto currentKit = snapshot[3];

		const synthLib::SMidiEvent deviceProgramChange(synthLib::MidiEventSource::Device,
			static_cast<uint8_t>(0xc0 | controller.getAutomationBaseChannel()),
			currentKit, 0);
		controller.parseMidiMessage(deviceProgramChange);
		require(controller.isAutomationSynchronized(),
			"outgoing firmware Program Change invalidated the Kit snapshot");

		const synthLib::SMidiEvent programChange(synthLib::MidiEventSource::Physical,
			static_cast<uint8_t>(0xc0 | controller.getAutomationBaseChannel()),
			currentKit, 0);
		controller.parseMidiMessage(programChange);
		require(!controller.isAutomationSynchronized(),
			"Program Change did not invalidate the Kit snapshot");
		require(_harness.synchronize(), "Program Change refresh timed out");
		require(processorListener.programChanges > 0,
			"Program Change refresh did not tell the host to rescan parameters");

		for(const auto status : {
			md::automation::sysex::StatusParameter::Kit,
			md::automation::sysex::StatusParameter::Pattern})
		{
			const pluginLib::SysEx setStatus{
				0xf0, 0x00, 0x20, 0x3c,
				static_cast<uint8_t>(_harness.model == md::MachineModel::Monomachine
					? 0x03 : 0x02),
				0x00, 0x71, static_cast<uint8_t>(status), currentKit, 0xf7};
			controller.parseSysexMessage(setStatus, synthLib::MidiEventSource::Physical);
			require(!controller.isAutomationSynchronized(),
				"external SET STATUS did not invalidate the Kit snapshot");
			require(_harness.synchronize(), "external SET STATUS refresh timed out");
		}
		_harness.audioProcessor.removeListener(&processorListener);
	}

	std::vector<uint8_t> withoutAutomationChunk(const std::vector<uint8_t>& _state)
	{
		std::vector<uint8_t> result;
		size_t position = 0;
		bool removed = false;
		while(position < _state.size())
		{
			require(_state.size() - position >= 12, "malformed saved chunk header");
			uint32_t length = 0;
			std::memcpy(&length, _state.data() + position + 8, sizeof(length));
			const auto chunkSize = static_cast<size_t>(12) + length;
			require(chunkSize <= _state.size() - position,
				"malformed saved chunk length");
			const auto isAutomation = std::memcmp(
				_state.data() + position, "AUTO", 4) == 0;
			if(isAutomation)
				removed = true;
			else
				result.insert(result.end(), _state.begin() + position,
					_state.begin() + position + chunkSize);
			position += chunkSize;
		}
		require(removed, "saved custom state did not contain AUTO chunk");
		return result;
	}

	void verifyStateContract(Harness& _harness)
	{
		auto& controller = _harness.controller;
		const auto baseline = controller.createAutomationSnapshot();
		require(!baseline.empty(), "automation snapshot was empty");
		require(controller.createAutomationSnapshot() == baseline,
			"unchanged automation snapshots were not deterministic");

		auto expectInvalid = [&](std::vector<uint8_t> _snapshot,
			const std::string& _description)
		{
			require(!controller.restoreAutomationSnapshot(_snapshot),
				"accepted invalid snapshot: " + _description);
			require(controller.createAutomationSnapshot() == baseline,
				"invalid snapshot mutated live state: " + _description);
		};

		expectInvalid({}, "empty");
		for(const auto length : {size_t{1}, size_t{5}, size_t{6},
			baseline.size() - 1})
			expectInvalid(std::vector<uint8_t>(baseline.begin(),
				baseline.begin() + length), "truncated");
		auto invalid = baseline;
		invalid.push_back(0);
		expectInvalid(invalid, "trailing byte");
		invalid = baseline;
		invalid[0] = 2;
		expectInvalid(invalid, "future version");
		invalid = baseline;
		invalid[1] ^= 1;
		expectInvalid(invalid, "wrong model");
		invalid = baseline;
		invalid[2] = 16;
		expectInvalid(invalid, "invalid MIDI base");
		invalid = baseline;
		invalid[3] = _harness.model == md::MachineModel::Monomachine ? 128 : 64;
		expectInvalid(invalid, "invalid Kit");
		invalid = baseline;
		invalid[4] = invalid[5] = 0;
		expectInvalid(invalid, "wrong parameter count");
		invalid = baseline;
		invalid[6] = 0xff;
		expectInvalid(invalid, "unknown address");
		invalid = baseline;
		invalid[10] = invalid[6];
		invalid[11] = invalid[7];
		invalid[12] = invalid[8];
		expectInvalid(invalid, "duplicate address");
		invalid = baseline;
		invalid[9] = 0xff;
		expectInvalid(invalid, "out-of-range value");

		require(controller.restoreAutomationSnapshot(baseline),
			"valid snapshot was rejected");
		require(controller.createAutomationSnapshot().empty(),
			"restoring a snapshot did not require firmware resynchronization");
		require(_harness.synchronize(), "valid snapshot replay timed out");
		require(controller.createAutomationSnapshot() == baseline,
			"snapshot round trip changed persisted automation state");

		std::vector<uint8_t> customState;
		_harness.processor.saveCustomData(customState);
		const auto legacyState = withoutAutomationChunk(customState);
		require(_harness.processor.loadCustomData(legacyState),
			"legacy custom state without AUTO was rejected");
		controller.onStateLoaded();
		require(_harness.synchronize(),
			"legacy state without AUTO did not resynchronize from firmware");
		require(!controller.createAutomationSnapshot().empty(),
			"legacy state did not reconstruct an automation snapshot");

		juce::MemoryBlock fullState;
		_harness.audioProcessor.getStateInformation(fullState);
		require(fullState.getSize() > 64, "full plugin state was unexpectedly small");
		for(const auto length : {size_t{1}, size_t{8}, fullState.getSize() / 2,
			fullState.getSize() - 1})
		{
			try
			{
				_harness.audioProcessor.setStateInformation(fullState.getData(),
					static_cast<int>(length));
			}
			catch(...)
			{
				require(false, "truncated plugin state escaped an exception");
			}
		}
		_harness.audioProcessor.setStateInformation(fullState.getData(),
			static_cast<int>(fullState.getSize()));
		require(_harness.synchronize(), "full plugin state recovery timed out");
	}

	void verifyRandomizedLifecycle(Harness& _harness)
	{
		auto allParameters = parameters(_harness, false);
		std::mt19937 random(0x4c494645u + static_cast<unsigned>(_harness.model));
		auto saved = _harness.controller.createAutomationSnapshot();
		require(!saved.empty(), "lifecycle test could not save initial state");
		const auto before = _harness.telemetry();
		for(int operation = 0; operation < 384; ++operation)
		{
			switch(random() % 7)
			{
			case 0:
			case 1:
			{
				auto& parameter = *allParameters[random() % allParameters.size()];
				hostWrite(parameter, static_cast<int>(random() & 0x7f));
				break;
			}
			case 2:
				_harness.controller.requestAutomationState();
				break;
			case 3:
				if(_harness.controller.isAutomationSynchronized())
				{
					const auto candidate =
						_harness.controller.createAutomationSnapshot();
					if(!candidate.empty())
						saved = candidate;
				}
				break;
			case 4:
				if(operation % 47 == 0)
					require(_harness.controller.restoreAutomationSnapshot(saved),
						"randomized lifecycle rejected a saved snapshot");
				break;
			case 5:
				if(_harness.controller.getAutomationBaseChannel() < 16)
				{
					auto& parameter = *allParameters[random() % allParameters.size()];
					const auto& description = parameter.getDescription();
					const md::automation::ParameterChange change{
						description.page, parameter.getPart(), description.index,
						static_cast<uint8_t>(random() & 0x7f)};
					if(const auto encoded = md::automation::encodeParameterChange(
						_harness.model, change,
						_harness.controller.getAutomationBaseChannel()))
						_harness.controller.parseControllerMessage({
							synthLib::MidiEventSource::Physical,
							(*encoded)[0], (*encoded)[1], (*encoded)[2]});
				}
				break;
			case 6:
			{
				auto malformed = saved;
				malformed.resize(random() % 6);
				require(!_harness.controller.restoreAutomationSnapshot(malformed),
					"randomized lifecycle accepted malformed state");
				break;
			}
			}
			_harness.process(static_cast<int>(1 + random() % 4));
			if((operation & 15) == 15)
				require(_harness.synchronize(),
					"randomized lifecycle synchronization timed out");
		}
		require(_harness.synchronize(), "final randomized lifecycle sync timed out");
		_harness.process(128);
		const auto after = _harness.telemetry();
		require(after.consumed > before.consumed,
			"randomized lifecycle produced no firmware MIDI traffic");
		require(after.overflows == before.overflows,
			"randomized lifecycle overflowed firmware MIDI RX");
	}

	void verifyFirmwareSoak(Harness& _harness, const size_t _writeCount)
	{
		auto allParameters = parameters(_harness, false);
		std::vector<int> expected(allParameters.size(), -1);
		std::mt19937 random(0x534f414bu + static_cast<unsigned>(_harness.model));
		const auto beforeCount =
			_harness.controller.getTransmittedAutomationChangeCount();
		const auto beforeMidi = _harness.telemetry();
		for(size_t write = 0; write < _writeCount; ++write)
		{
			const auto index = static_cast<size_t>(random() % allParameters.size());
			const auto value = static_cast<int>(random() & 0x7f);
			hostWrite(*allParameters[index], value);
			expected[index] = value;
			if((write & 7) == 7)
				_harness.process(4);
		}
		// Firmware echoes and parameter normalization arrive asynchronously. Require
		// the final Level values to remain correct across a sustained drain window,
		// rather than sampling the cache at one scheduler-dependent instant.
		auto levelsMatch = [&]()
		{
			for(size_t index = 0; index < allParameters.size(); ++index)
			{
				if(expected[index] >= 0
					&& allParameters[index]->getDescription().name == "Level"
					&& allParameters[index]->getUnnormalizedValue() != expected[index])
					return false;
			}
			return true;
		};
		size_t stableBlocks = 0;
		for(size_t block = 0; block < 3000 && stableBlocks < 128; ++block)
		{
			_harness.process(1);
			stableBlocks = levelsMatch() ? stableBlocks + 1 : 0;
		}
		require(stableBlocks == 128,
			"soak final Level values did not settle after firmware echoes");
		const auto afterMidi = _harness.telemetry();
		const auto afterCount =
			_harness.controller.getTransmittedAutomationChangeCount();
		if(afterCount != beforeCount + _writeCount)
			throw std::runtime_error("soak transmitted "
				+ std::to_string(afterCount - beforeCount) + " of "
				+ std::to_string(_writeCount) + " explicit host writes");
		require(afterMidi.consumed >= beforeMidi.consumed + _writeCount * 3,
			"firmware did not consume the complete soak stream");
		require(afterMidi.overflows == beforeMidi.overflows,
			"soak overflowed firmware MIDI RX");
		require(afterMidi.contentionDrops == beforeMidi.contentionDrops,
			"soak dropped firmware output on controller lock contention");
		require(afterMidi.capacityDrops == beforeMidi.capacityDrops,
			"soak dropped firmware output at controller queue capacity");
	}

	void verifyModel(const md::MachineModel _model, const size_t _soakWrites)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
		{
			std::cout << "mdAutomationRobustnessTest: SKIP " << modelName(_model)
				<< " (firmware unavailable)\n";
			return;
		}
		verifyQueuedPreBootWrites(harness);
		verifyExternalNotifications(harness);
		verifyStateContract(harness);
		verifyRandomizedLifecycle(harness);
		verifyFirmwareSoak(harness, _soakWrites);
		std::cout << "mdAutomationRobustnessTest: " << modelName(_model)
			<< " PASS (" << _soakWrites << " soak writes)\n";
	}

	void verifyConcurrentIsolation(const size_t _writeCount)
	{
		Harness mdHarness(md::MachineModel::Machinedrum);
		Harness mmHarness(md::MachineModel::Monomachine);
		if(!mdHarness.hasLocalFirmware() || !mmHarness.hasLocalFirmware())
		{
			std::cout << "mdAutomationRobustnessTest: SKIP multi-instance"
				" (firmware unavailable)\n";
			return;
		}
		mdHarness.prepare();
		mmHarness.prepare();
		std::atomic<bool> start{false};
		std::exception_ptr mdError;
		std::exception_ptr mmError;
		auto run = [&start, _writeCount](Harness& _harness, std::exception_ptr& _error,
			const uint32_t _seed)
		{
			try
			{
				while(!start.load(std::memory_order_acquire))
					std::this_thread::yield();
				require(_harness.synchronize(),
					"concurrent instance failed to synchronize");
				auto allParameters = parameters(_harness, false);
				allParameters.erase(std::remove_if(allParameters.begin(),
					allParameters.end(), [](const pluginLib::Parameter* const _parameter)
					{
						return _parameter->getDescription().name != "Level";
					}), allParameters.end());
				require(!allParameters.empty(),
					"concurrent instance has no stable Level parameters");
				std::mt19937 random(_seed);
				const auto before =
					_harness.controller.getTransmittedAutomationChangeCount();
				auto expectedDigest =
					_harness.controller.getTransmittedAutomationDigest();
				for(size_t write = 0; write < _writeCount; ++write)
				{
					const auto index = static_cast<size_t>(random() % allParameters.size());
					const auto value = static_cast<int>(random() & 0x7f);
					const auto& description = allParameters[index]->getDescription();
					const auto encoded = md::automation::encodeParameterChange(
						_harness.model,
						{description.page, allParameters[index]->getPart(),
							description.index, static_cast<uint8_t>(value)},
						_harness.controller.getAutomationBaseChannel());
					require(encoded.has_value(),
						"concurrent expected automation change did not encode");
					expectedDigest = appendDigest(expectedDigest, *encoded);
					hostWrite(*allParameters[index], value);
					if((write & 7) == 7)
						_harness.process(4);
				}
				_harness.process(192);
				require(_harness.controller.getTransmittedAutomationChangeCount()
					== before + _writeCount,
					"concurrent instance lost or gained automation writes");
				require(_harness.controller.getTransmittedAutomationDigest()
					== expectedDigest,
					std::string("concurrent ") + modelName(_harness.model)
					+ " automation message sequence was contaminated");
				const auto snapshot =
					_harness.controller.createAutomationSnapshot();
				require(snapshot.size() == 6
					+ _harness.audioProcessor.getParameters().size() * 4,
					std::string("concurrent ") + modelName(_harness.model)
					+ " snapshot shape was contaminated");
				require(_harness.telemetry().overflows == 0,
					"concurrent instance overflowed firmware MIDI RX");
			}
			catch(...)
			{
				_error = std::current_exception();
			}
		};

		std::thread mdThread(run, std::ref(mdHarness), std::ref(mdError), 0x4d444d44u);
		std::thread mmThread(run, std::ref(mmHarness), std::ref(mmError), 0x4d4d4d4du);
		start.store(true, std::memory_order_release);
		mdThread.join();
		mmThread.join();
		if(mdError)
			std::rethrow_exception(mdError);
		if(mmError)
			std::rethrow_exception(mmError);
		std::cout << "mdAutomationRobustnessTest: concurrent MD/MM PASS ("
			<< _writeCount << " writes per instance)\n";
	}
}

int main(const int _argc, const char* const* _argv)
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		size_t soakWrites = 8192;
		size_t multiWrites = 2048;
		bool mdOnly = false;
		bool mmOnly = false;
		bool noMulti = false;
		bool multiOnly = false;
		for(int argument = 1; argument < _argc; ++argument)
		{
			const std::string value(_argv[argument]);
			if(value == "--md")
				mdOnly = true;
			else if(value == "--mm")
				mmOnly = true;
			else if(value == "--no-multi")
				noMulti = true;
			else if(value == "--multi-only")
				multiOnly = true;
			else if(value.rfind("--soak-writes=", 0) == 0)
				soakWrites = static_cast<size_t>(std::stoul(value.substr(14)));
			else if(value.rfind("--multi-writes=", 0) == 0)
				multiWrites = static_cast<size_t>(std::stoul(value.substr(15)));
		}
		if(!multiOnly)
		{
			if(!mmOnly)
				verifyModel(md::MachineModel::Machinedrum, soakWrites);
			if(!mdOnly)
				verifyModel(md::MachineModel::Monomachine, soakWrites);
		}
		if(!noMulti && !mdOnly && !mmOnly)
			verifyConcurrentIsolation(multiWrites);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdAutomationRobustnessTest: " << error.what() << '\n';
		return 1;
	}
}
