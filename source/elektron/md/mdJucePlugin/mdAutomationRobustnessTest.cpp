#include "mdAutomationTestSupport.h"

#include "mdLib/mdsysexautomation.h"
#include "synthLib/midiTypes.h"

#include "juce_events/juce_events.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
	using namespace mdAutomationTest;

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

	pluginLib::SysEx setStatus(const md::MachineModel _model,
		const md::automation::sysex::StatusParameter _parameter,
		const uint8_t _value)
	{
		return {0xf0, 0x00, 0x20, 0x3c,
			static_cast<uint8_t>(_model == md::MachineModel::Monomachine ? 0x03 : 0x02),
			0x00, 0x71, static_cast<uint8_t>(_parameter), _value, 0xf7};
	}

	pluginLib::SysEx statusResponse(const md::MachineModel _model,
		const md::automation::sysex::StatusParameter _parameter,
		const uint8_t _value)
	{
		auto result = setStatus(_model, _parameter, _value);
		result[6] = 0x72;
		return result;
	}

	int snapshotValue(const std::vector<uint8_t>& _snapshot,
		const pluginLib::Parameter& _parameter)
	{
		const auto& description = _parameter.getDescription();
		for(size_t position = 6; position + 3 < _snapshot.size(); position += 4)
		{
			if(_snapshot[position] == description.page
				&& _snapshot[position + 1] == _parameter.getPart()
				&& _snapshot[position + 2] == description.index)
				return _snapshot[position + 3];
		}
		return -1;
	}

	void primeSyntheticSnapshot(Harness& _harness)
	{
		auto& controller = _harness.controller;
		controller.requestAutomationState();
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Kit, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, 23),
			synthLib::MidiEventSource::Device);
		require(controller.isAutomationSynchronized(),
			"synthetic architecture snapshot did not synchronize");
	}

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
		_harness.process(512);
		require(_harness.telemetry().overflows == 0,
			"pre-boot queue overflowed firmware MIDI RX");
		for(const auto& [parameter, value] : expected)
		{
			if(parameter->getDescription().name == "Level")
				require(parameter->getUnnormalizedValue() == value,
					"late startup dump overwrote a flushed pre-boot host value");
		}
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

	void verifyCorrelatedDumpsAndStrictStatus(Harness& _harness)
	{
		auto& controller = _harness.controller;
		const auto baseline = controller.createAutomationSnapshot();
		require(!baseline.empty(), "missing correlated-dump test baseline");
		const auto currentKit = baseline[3];
		const auto wrongKit = static_cast<uint8_t>((currentKit + 1)
			% (_harness.model == md::MachineModel::Monomachine ? 128 : 64));

		// Valid but unsolicited dumps, including a duplicate for the active slot,
		// must not replace the authoritative snapshot.
		controller.parseSysexMessage(makeKitDump(_harness.model, wrongKit, 91),
			synthLib::MidiEventSource::Physical);
		controller.parseSysexMessage(makeKitDump(_harness.model, currentKit, 92),
			synthLib::MidiEventSource::Physical);
		require(controller.createAutomationSnapshot() == baseline,
			"unsolicited Kit dump replaced the active snapshot");

		// Strict framing matters at the controller boundary: malformed lookalikes
		// must not invalidate an otherwise-ready snapshot.
		const auto validSet = setStatus(_harness.model,
			md::automation::sysex::StatusParameter::Kit, currentKit);
		for(const auto position : {size_t{1}, size_t{2}, size_t{3}, size_t{4},
			size_t{5}, size_t{6}, size_t{9}})
		{
			auto malformed = validSet;
			malformed[position] ^= 1;
			controller.parseSysexMessage(malformed,
				synthLib::MidiEventSource::Physical);
			require(controller.isAutomationSynchronized()
				&& controller.createAutomationSnapshot() == baseline,
				"malformed SET STATUS invalidated synchronization");
		}

		// Once a refresh is outstanding, a dump for a different slot is stale and
		// cannot complete synchronization.
		const synthLib::SMidiEvent programChange(synthLib::MidiEventSource::Physical,
			static_cast<uint8_t>(0xc0 | controller.getAutomationBaseChannel()),
			currentKit, 0);
		controller.parseMidiMessage(programChange);
		require(controller.createAutomationSnapshot().empty(),
			"snapshot escaped while Kit refresh was incomplete");
		controller.parseSysexMessage(makeKitDump(_harness.model, wrongKit, 93),
			synthLib::MidiEventSource::Physical);
		require(!controller.isAutomationSynchronized()
			&& controller.createAutomationSnapshot().empty(),
			"wrong-slot Kit dump completed synchronization");
		require(_harness.synchronize(), "correlated Kit refresh timed out");

		// Exercise the same correlation for Globals and prove that a duplicate late
		// dump cannot alter the active base channel.
		const auto originalBase = controller.getAutomationBaseChannel();
		controller.parseSysexMessage(setStatus(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Physical);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 1,
			static_cast<uint8_t>((originalBase + 1) & 0x0f)),
			synthLib::MidiEventSource::Physical);
		require(!controller.isAutomationSynchronized(),
			"wrong-slot Global dump completed synchronization");
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, originalBase),
			synthLib::MidiEventSource::Physical);
		require(controller.isAutomationSynchronized(),
			"expected Global dump did not complete synchronization");
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0,
			static_cast<uint8_t>((originalBase + 1) & 0x0f)),
			synthLib::MidiEventSource::Physical);
		require(controller.getAutomationBaseChannel() == originalBase,
			"duplicate late Global dump changed the base channel");
	}

	void verifyMidiNoneReplay(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& probe = *parameters(_harness, false).front();
		const auto originalBase = controller.getAutomationBaseChannel();
		require(originalBase < 16, "MIDI NONE replay test needs an enabled base");

		controller.parseSysexMessage(setStatus(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Physical);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 0x7f),
			synthLib::MidiEventSource::Physical);
		require(controller.isAutomationSynchronized()
			&& controller.getAutomationBaseChannel() == 0x7f,
			"MIDI NONE was not accepted as a complete readable snapshot");

		const auto before = controller.getTransmittedAutomationChangeCount();
		const auto value = static_cast<int>((probe.getUnnormalizedValue() + 41) & 0x7f);
		hostWrite(probe, value);
		controller.processRealtimeParameterChanges(64);
		const auto noneSnapshot = controller.createAutomationSnapshot();
		require(snapshotValue(noneSnapshot, probe) == value,
			"MIDI NONE snapshot lost the newest host value");
		require(controller.getTransmittedAutomationChangeCount() == before,
			"MIDI NONE transmitted an unroutable host write");

		controller.parseSysexMessage(setStatus(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Physical);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, originalBase),
			synthLib::MidiEventSource::Physical);
		require(controller.isAutomationSynchronized()
			&& controller.getTransmittedAutomationChangeCount() == before + 1,
			"host intent queued during MIDI NONE was not replayed exactly once");
		require(snapshotValue(controller.createAutomationSnapshot(), probe) == value,
			"re-enabled MIDI snapshot lost the replayed host value");

		// Return to the actual firmware-selected Global after the synthetic probes.
		controller.requestAutomationState();
		require(_harness.synchronize(), "post-NONE firmware resynchronization timed out");
	}

	void verifyOrderedIntentArchitecture(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& probe = *parameters(_harness, false).front();
		const auto baseline = controller.createAutomationSnapshot();
		require(!baseline.empty(), "missing ordered-intent test baseline");
		const auto currentKit = baseline[3];

		// Reproduce the timer ordering that previously lost a host write: the Kit
		// dump is parsed before the controller's host-automation queue is drained.
		const synthLib::SMidiEvent programChange(synthLib::MidiEventSource::Physical,
			static_cast<uint8_t>(0xc0 | controller.getAutomationBaseChannel()),
			currentKit, 0);
		controller.parseMidiMessage(programChange);
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Kit, currentKit),
			synthLib::MidiEventSource::Device);
		const auto hostValue = static_cast<int>((probe.getUnnormalizedValue() + 37) & 0x7f);
		hostWrite(probe, hostValue);
		controller.parseSysexMessage(makeKitDump(_harness.model, currentKit,
			static_cast<uint8_t>((hostValue + 19) & 0x7f)),
			synthLib::MidiEventSource::Device);
		require(controller.isAutomationSynchronized(),
			"Kit dump did not complete ordered-intent synchronization");
		require(probe.getUnnormalizedValue() == hostValue
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == hostValue,
			"Kit dump overwrote an undrained host publication");

		// A direct MIDI edit observed after the dump request is newer authoritative
		// state even though it does not need delivery. The request watermark keeps a
		// subsequently arriving stale dump from rolling it back.
		controller.parseMidiMessage(programChange);
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Kit, currentKit),
			synthLib::MidiEventSource::Device);
		const auto externalValue = (hostValue + 7) & 0x7f;
		const auto& description = probe.getDescription();
		const auto external = md::automation::encodeParameterChange(_harness.model,
			{description.page, probe.getPart(), description.index,
				static_cast<uint8_t>(externalValue)}, controller.getAutomationBaseChannel());
		require(external.has_value(), "post-request MIDI value did not encode");
		controller.parseControllerMessage({synthLib::MidiEventSource::Physical,
			(*external)[0], (*external)[1], (*external)[2]});
		controller.parseSysexMessage(makeKitDump(_harness.model, currentKit,
			static_cast<uint8_t>((externalValue + 13) & 0x7f)),
			synthLib::MidiEventSource::Device);
		require(probe.getUnnormalizedValue() == externalValue
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == externalValue,
			"Kit dump overwrote a newer post-request MIDI publication");

		// Host and UI writes now share one publication order. The older queued host
		// value must never arrive after the newer synchronous UI value.
		const auto olderHostValue = (hostValue + 11) & 0x7f;
		const auto newerUiValue = (hostValue + 29) & 0x7f;
		const auto beforeUi = controller.getTransmittedAutomationChangeCount();
		hostWrite(probe, olderHostValue);
		probe.setUnnormalizedValue(newerUiValue, pluginLib::Parameter::Origin::Ui);
		require(controller.getTransmittedAutomationChangeCount() == beforeUi + 1,
			"ordered UI publication did not coalesce the older host value");
		require(probe.getUnnormalizedValue() == newerUiValue
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == newerUiValue,
			"older host publication won after a newer UI change");

		// Overflow drops queue hints only. The latest atomic publication must remain
		// immediately snapshot-visible and must be delivered once by the slot scan.
		controller.processRealtimeParameterChanges(1024);
		const auto beforeOverflow = controller.getRealtimeAutomationOverflowCount();
		const auto beforeOverflowTransmit =
			controller.getTransmittedAutomationChangeCount();
		int overflowValue = newerUiValue;
		for(size_t write = 0; write < 4352; ++write)
		{
			overflowValue = static_cast<int>((write * 23 + 17) & 0x7f);
			hostWrite(probe, overflowValue);
		}
		require(controller.getRealtimeAutomationOverflowCount() > beforeOverflow,
			"overflow probe did not exceed the bounded hint queue");
		require(snapshotValue(controller.createAutomationSnapshot(), probe) == overflowValue,
			"overflow lost the authoritative latest publication");
		for(int pass = 0; pass < 8; ++pass)
			controller.processRealtimeParameterChanges(1024);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeOverflowTransmit + 1,
			"overflow did not coalesce to exactly one latest delivery");
		require(snapshotValue(controller.createAutomationSnapshot(), probe) == overflowValue,
			"overflow delivery changed the authoritative snapshot");
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
		auto& probe = *parameters(_harness, false).front();
		const auto immediateValue = static_cast<int>(
			(probe.getUnnormalizedValue() + 29) & 0x7f);
		hostWrite(probe, immediateValue);
		const auto immediate = controller.createAutomationSnapshot();
		require(snapshotValue(immediate, probe) == immediateValue,
			"snapshot did not publish an undrained host write atomically");
		_harness.process(32);
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

	void verifyModel(const md::MachineModel _model)
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
		verifyCorrelatedDumpsAndStrictStatus(harness);
		verifyMidiNoneReplay(harness);
		verifyOrderedIntentArchitecture(harness);
		verifyStateContract(harness);
		verifyRandomizedLifecycle(harness);
		std::cout << "mdAutomationRobustnessTest: " << modelName(_model)
			<< " PASS\n";
	}

	void verifyArchitecture(const md::MachineModel _model)
	{
		Harness harness(_model);
		primeSyntheticSnapshot(harness);
		verifyOrderedIntentArchitecture(harness);
		std::cout << "mdAutomationRobustnessTest: " << modelName(_model)
			<< " ARCHITECTURE PASS\n";
	}

}

int main(const int _argc, const char* const* _argv)
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		bool mdOnly = false;
		bool mmOnly = false;
		bool architectureOnly = false;
		for(int argument = 1; argument < _argc; ++argument)
		{
			const std::string value(_argv[argument]);
			if(value == "--md")
				mdOnly = true;
			else if(value == "--mm")
				mmOnly = true;
			else if(value == "--architecture-only")
				architectureOnly = true;
		}
		if(architectureOnly)
		{
			if(!mmOnly)
				verifyArchitecture(md::MachineModel::Machinedrum);
			if(!mdOnly)
				verifyArchitecture(md::MachineModel::Monomachine);
			return 0;
		}
		if(!mmOnly)
			verifyModel(md::MachineModel::Machinedrum);
		if(!mdOnly)
			verifyModel(md::MachineModel::Monomachine);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdAutomationRobustnessTest: " << error.what() << '\n';
		return 1;
	}
}
