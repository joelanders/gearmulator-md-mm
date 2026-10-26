#include "mdAutomationTestSupport.h"

#include "mdLib/mdsysexautomation.h"
#include "synthLib/midiTypes.h"

#include "juce_events/juce_events.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace realtimeAllocationProbe
{
	thread_local bool enabled = false;
	thread_local std::size_t allocations = 0;

	void observedAllocation() noexcept
	{
		if(enabled)
			++allocations;
	}
}

// This executable owns its allocation functions so a thread-local probe can
// enforce the host-write realtime contract without affecting production code.
void* operator new(const std::size_t _size)
{
	realtimeAllocationProbe::observedAllocation();
	if(auto* const result = std::malloc(_size == 0 ? 1 : _size))
		return result;
	throw std::bad_alloc();
}

void* operator new[](const std::size_t _size)
{
	return ::operator new(_size);
}

void operator delete(void* const _memory) noexcept
{
	std::free(_memory);
}

void operator delete[](void* const _memory) noexcept
{
	std::free(_memory);
}

void operator delete(void* const _memory, std::size_t) noexcept
{
	std::free(_memory);
}

void operator delete[](void* const _memory, std::size_t) noexcept
{
	std::free(_memory);
}

namespace mdJucePlugin
{
	struct ControllerAutomationTestAccess
	{
		static void useSyntheticFirmware(Controller& _controller)
		{
			_controller.m_syntheticFirmwareReadyForTests = true;
		}

		static void expireKitRequest(Controller& _controller)
		{
			// Age the request, then exercise the real timer/recovery path without
			// sleeping or replacing the controller's protocol decisions.
			_controller.m_kitSynchronization.dumpRequestSent(
				Controller::milliseconds() - 2001);
			_controller.onControllerTimer();
		}

		static void pollNow(Controller& _controller)
		{
			_controller.m_lastStatePollMs.store(Controller::milliseconds() - 5001);
			_controller.onControllerTimer();
		}

		static void tick(Controller& _controller)
		{
			_controller.onControllerTimer();
		}
	};
}

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
		if(_snapshot.size() < 6)
			return -1;
		const auto headerSize = _snapshot[0] == 1 ? size_t{6} : size_t{7};
		const auto entrySize = _snapshot[0] == 1 ? size_t{4} : size_t{5};
		const auto& description = _parameter.getDescription();
		for(size_t position = headerSize; position + 3 < _snapshot.size();
			position += entrySize)
		{
			if(_snapshot[position] == description.page
				&& _snapshot[position + 1] == _parameter.getPart()
				&& _snapshot[position + 2] == description.index)
				return _snapshot[position + 3];
		}
		return -1;
	}

	bool snapshotIsComplete(const std::vector<uint8_t>& _snapshot)
	{
		return _snapshot.size() >= 6 && (_snapshot[0] == 1
			|| (_snapshot.size() >= 7 && _snapshot[0] == 2
				&& (_snapshot[6] & 1) != 0));
	}

	void primeSyntheticSnapshot(Harness& _harness,
		const uint8_t _finalKitValue = 23)
	{
		auto& controller = _harness.controller;
		controller.requestAutomationState();
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 7),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, 99),
			synthLib::MidiEventSource::Device);
		require(!controller.isAutomationSynchronized()
			&& !controller.hasAutomationGlobalSnapshot()
			&& !controller.hasAutomationKitSnapshot(),
			"unsolicited startup dumps completed synchronization before status");
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Kit, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 1, 7),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 1, 99),
			synthLib::MidiEventSource::Device);
		require(!controller.isAutomationSynchronized()
			&& !controller.hasAutomationGlobalSnapshot()
			&& !controller.hasAutomationKitSnapshot(),
			"wrong-slot startup dumps satisfied correlated requests");
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 0),
			synthLib::MidiEventSource::Device);
		require(!controller.isAutomationSynchronized()
			&& controller.hasAutomationGlobalSnapshot()
			&& !controller.hasAutomationKitSnapshot(),
			"one correlated startup reply completed a partial snapshot");
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, _finalKitValue),
			synthLib::MidiEventSource::Device);
		require(controller.isAutomationSynchronized(),
			"synthetic architecture snapshot did not synchronize");
	}

	void verifyRetriedKitSynchronization(const md::MachineModel _model,
		const bool _kitFirst)
	{
		using Access = mdJucePlugin::ControllerAutomationTestAccess;
		using Status = md::automation::sysex::StatusParameter;
		Harness harness(_model);
		auto& controller = harness.controller;
		Access::useSyntheticFirmware(controller);
		auto* const probe = parameters(harness, false).front();
		const auto reply = [&](const pluginLib::SysEx& message)
		{
			controller.parseSysexMessage(message, synthLib::MidiEventSource::Device);
		};

		// Initial baseline, ordinary same-slot inspection, explicit same-slot
		// reload, and changed Kit must retain their distinct purposes on retry.
		for(int scenario = 0; scenario < 6; ++scenario)
		{
			const uint8_t slot = scenario >= 3 ? 1 : 0;
			const uint8_t stored = static_cast<uint8_t>(23 + scenario * 7);
			if(scenario == 1)
			{
				hostWrite(*probe, 91);
				Access::tick(controller);
			}
			if(scenario == 2 || scenario >= 4)
				controller.onStateLoaded();
			else
				controller.requestAutomationState();
			reply(statusResponse(_model, Status::Global, 0));
			reply(statusResponse(_model, Status::Kit, slot));
			if(!_kitFirst)
				reply(makeGlobalDump(_model, 0, 0));

			if(scenario == 4)
				hostWrite(*probe, 87);
			if(scenario == 5)
			{
				const auto& description = probe->getDescription();
				const auto encoded = md::automation::encodeParameterChange(_model,
					{description.page, probe->getPart(), description.index, 83}, 0);
				require(encoded.has_value(), "retry probe has no MIDI encoding");
				require(controller.parseControllerMessage({synthLib::MidiEventSource::Physical,
					(*encoded)[0], (*encoded)[1], (*encoded)[2]}),
					"direct edit during pending Kit request was rejected");
			}
			for(int retry = 0; retry < 2; ++retry)
			{
				const auto before = controller.getSynchronizationRequestCount();
				Access::expireKitRequest(controller);
				require(controller.getSynchronizationRequestCount() > before,
					"expired Kit request did not issue a new status barrier");
				reply(makeKitDump(_model, slot, 101));
				require(!controller.hasAutomationKitSnapshot()
					&& !snapshotIsComplete(controller.createAutomationSnapshot()),
					"late pre-status Kit reply completed a retry");
				reply(statusResponse(_model, Status::Kit, slot));
				reply(makeKitDump(_model, static_cast<uint8_t>(slot + 1), 102));
				require(!controller.hasAutomationKitSnapshot(),
					"wrong-slot Kit reply completed a retry");
			}
			reply(makeKitDump(_model, slot, stored));
			if(_kitFirst)
			{
				require(!controller.isAutomationSynchronized(),
					"Kit-only retry completion declared readiness without Global");
				reply(makeGlobalDump(_model, 0, 0));
			}
			const auto snapshot = controller.createAutomationSnapshot();
			require(controller.isAutomationSynchronized() && snapshotIsComplete(snapshot),
				"correlated retry replies did not complete synchronization");
			const auto expected = scenario == 1 ? 91 : scenario == 4 ? 87
				: scenario == 5 ? 83 : stored;
			require(controller.getLastFirmwareKitValue(*probe) == stored
				&& snapshotValue(snapshot, *probe) == expected
				&& probe->getUnnormalizedValue() == expected,
				"retried Kit lost baseline/live/host intent in scenario "
					+ std::to_string(scenario));
			// Check the whole dump, not just one knob or an aggregate audio peak.
			if(scenario != 1)
			{
				for(auto* parameter : parameters(harness, false))
				{
					if(controller.getLastFirmwareKitValue(*parameter) < 0)
						continue; // not represented by this model's Kit dump
					const auto value = parameter == probe ? expected
						: controller.getLastFirmwareKitValue(*parameter);
					require(snapshotValue(snapshot, *parameter) == value,
						"retried Kit snapshot disagrees with firmware baseline");
				}
			}
			reply(makeKitDump(_model, slot, 103));
			require(controller.createAutomationSnapshot() == snapshot,
				"duplicate Kit dump changed the completed retry snapshot");
		}
	}

	void verifyPeriodicControllerPolling(const md::MachineModel _model)
	{
		using Access = mdJucePlugin::ControllerAutomationTestAccess;
		using Status = md::automation::sysex::StatusParameter;
		Harness harness(_model);
		auto& controller = harness.controller;
		Access::useSyntheticFirmware(controller);
		primeSyntheticSnapshot(harness);
		auto& probe = *parameters(harness, false).front();
		const auto reply = [&](const pluginLib::SysEx& message)
		{
			controller.parseSysexMessage(message, synthLib::MidiEventSource::Device);
		};
		const auto poll = [&]
		{
			const auto before = controller.getSynchronizationRequestCount();
			Access::pollNow(controller);
			require(controller.getSynchronizationRequestCount() == before + 2,
				"ready timer failed to poll both Global and Kit");
			Access::tick(controller);
			require(controller.getSynchronizationRequestCount() == before + 2,
				"ready timer repeated a poll before its deadline");
		};

		hostWrite(probe, 91);
		Access::tick(controller);
		poll();
		const auto before = controller.getSynchronizationRequestCount();
		reply(statusResponse(_model, Status::Kit, 0));
		require(controller.getSynchronizationRequestCount() == before,
			"same-Kit periodic poll requested a stored dump");
		reply(statusResponse(_model, Status::Global, 0));
		require(controller.getSynchronizationRequestCount() == before + 1,
			"same-slot Global was not refreshed by periodic polling");
		reply(makeGlobalDump(_model, 0, 0x7f));
		require(controller.isAutomationSynchronized()
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == 91,
			"periodic Global refresh lost live Kit edits");
		const auto transmitted = controller.getTransmittedAutomationChangeCount();
		hostWrite(probe, 87);
		Access::tick(controller);
		require(controller.getTransmittedAutomationChangeCount() == transmitted,
			"MIDI NONE transmitted queued host intent");
		poll();
		reply(statusResponse(_model, Status::Global, 0));
		reply(statusResponse(_model, Status::Kit, 0));
		reply(makeGlobalDump(_model, 0, 3));
		require(controller.getTransmittedAutomationChangeCount() > transmitted
			&& controller.getAutomationBaseChannel() == 3
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == 87,
			"periodic poll did not recover pending intent after MIDI NONE");

		poll();
		reply(statusResponse(_model, Status::Global, 0));
		reply(makeGlobalDump(_model, 0, 3));
		reply(statusResponse(_model, Status::Kit, 1));
		Access::expireKitRequest(controller);
		reply(statusResponse(_model, Status::Kit, 1));
		reply(makeKitDump(_model, 1, 42));
		require(controller.isAutomationSynchronized()
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == 42,
			"periodic changed-Kit retry did not publish the new baseline");
	}

	void verifyAdversarialRestoreSynchronization(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& probe = *parameters(_harness, false).front();
		auto restored = controller.createAutomationSnapshot();
		require(!restored.empty(), "missing restore synchronization baseline");
		const auto desired = static_cast<uint8_t>(
			(static_cast<int>(probe.getUnnormalizedValue()) + 61) & 0x7f);
		const auto& description = probe.getDescription();
		bool changed = false;
		const auto headerSize = restored[0] == 1 ? size_t{6} : size_t{7};
		const auto entrySize = restored[0] == 1 ? size_t{4} : size_t{5};
		for(size_t position = headerSize; position < restored.size();
			position += entrySize)
		{
			if(restored[position] == description.page
				&& restored[position + 1] == probe.getPart()
				&& restored[position + 2] == description.index)
			{
				restored[position + 3] = desired;
				changed = true;
				break;
			}
		}
		require(changed && controller.restoreAutomationSnapshot(restored),
			"adversarial restore snapshot was rejected");
		controller.onStateLoaded();

		// Replies left over from the retired firmware generation have no request
		// identity in this epoch and must not complete the restored snapshot.
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 3),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, 4),
			synthLib::MidiEventSource::Device);
		require(!controller.isAutomationSynchronized()
			&& !snapshotIsComplete(controller.createAutomationSnapshot()),
			"pre-status replies completed state restore synchronization");

		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Kit, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 1, 3),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 1, 4),
			synthLib::MidiEventSource::Device);
		require(!controller.isAutomationSynchronized(),
			"wrong-slot replies completed state restore synchronization");

		// Complete in the opposite order from startup and interleave another stale
		// reply. The restored dirty publication must win over the Kit payload.
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, 12),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 1, 13),
			synthLib::MidiEventSource::Device);
		require(!controller.isAutomationSynchronized()
			&& probe.getUnnormalizedValue() == desired,
			"Kit-first restore completion lost restored intent");
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 0),
			synthLib::MidiEventSource::Device);
		require(controller.isAutomationSynchronized()
			&& snapshotValue(controller.createAutomationSnapshot(), probe) == desired,
			"correlated restore replies did not publish the restored snapshot");

		const auto completed = controller.createAutomationSnapshot();
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 9),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, 10),
			synthLib::MidiEventSource::Device);
		require(controller.createAutomationSnapshot() == completed,
			"late duplicate replies mutated the completed restore snapshot");
	}

	void verifyConcurrentPublicationArchitecture(Harness& _harness)
	{
		auto allParameters = parameters(_harness, false);
		require(allParameters.size() >= 4,
			"concurrent publication architecture needs four parameters");
		constexpr int ProducerCount = 4;
		constexpr int WritesPerProducer = 2048;
		std::array<int, ProducerCount> expected{};
		std::atomic<bool> start{false};
		std::atomic<int> active{ProducerCount};
		std::array<std::thread, ProducerCount> producers;
		std::thread drainer([&]
		{
			while(!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			while(active.load(std::memory_order_acquire) > 0)
				_harness.controller.processRealtimeParameterChanges(32);
			for(size_t pass = 0; pass < allParameters.size(); ++pass)
				_harness.controller.processRealtimeParameterChanges(32);
		});
		for(int producer = 0; producer < ProducerCount; ++producer)
		{
			producers[producer] = std::thread([&, producer]
			{
				while(!start.load(std::memory_order_acquire))
					std::this_thread::yield();
				for(int write = 0; write < WritesPerProducer; ++write)
				{
					const auto value = (producer * 31 + write * 17) & 0x7f;
					expected[producer] = value;
					hostWrite(*allParameters[producer], value);
				}
				active.fetch_sub(1, std::memory_order_release);
			});
		}
		start.store(true, std::memory_order_release);
		for(auto& producer : producers)
			producer.join();
		drainer.join();

		const auto snapshot = _harness.controller.createAutomationSnapshot();
		require(!snapshot.empty(),
			"concurrent publication invalidated the synchronized snapshot");
		for(int producer = 0; producer < ProducerCount; ++producer)
		{
			require(snapshotValue(snapshot, *allParameters[producer])
				== expected[producer],
				"concurrent producer lost its latest authoritative publication");
		}
	}

	void verifyQueuedPreBootWrites(Harness& _harness)
	{
		require(!firmwareMidiReady(_harness),
			"explicit firmware MIDI readiness was true before boot processing");
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
		require(firmwareMidiReady(_harness),
			"automation synchronized without explicit firmware MIDI readiness");
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
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
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
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
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
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
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

		// Ordinary DAW writes retain their exact FIFO sequence even when two values
		// for one address are published before the audio callback drains them.
		const auto firstHostValue = (hostValue + 3) & 0x7f;
		const auto secondHostValue = (hostValue + 5) & 0x7f;
		const auto beforeHostSequence = controller.getTransmittedAutomationChangeCount();
		hostWrite(probe, firstHostValue);
		hostWrite(probe, secondHostValue);
		controller.processRealtimeParameterChanges(64);
		require(controller.getTransmittedAutomationChangeCount()
			== beforeHostSequence + 2,
			"ordered DAW publications did not preserve both explicit writes");
		require(snapshotValue(controller.createAutomationSnapshot(), probe)
			== secondHostValue,
			"ordered DAW publications did not retain the latest snapshot value");

		// Host and UI writes share one publication order, but a synchronous UI edit
		// intentionally supersedes an older queued host value. It must never arrive
		// after the newer UI value.
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
		int recoveryPasses = 0;
		while(controller.getTransmittedAutomationChangeCount()
			== beforeOverflowTransmit && recoveryPasses < 2)
		{
			controller.processRealtimeParameterChanges(1024);
			++recoveryPasses;
		}
		require(controller.getTransmittedAutomationChangeCount()
			== beforeOverflowTransmit + 1,
			"overflow did not coalesce to exactly one latest delivery");
		require(recoveryPasses <= 2,
			"overflow recovery exceeded the reserved slot-scan bound");
		require(snapshotValue(controller.createAutomationSnapshot(), probe) == overflowValue,
			"overflow delivery changed the authoritative snapshot");

		// Even the smallest caller budget alternates progress through the rotating
		// slot scan and the stale hint backlog. Two slot-table traversals are therefore
		// an absolute recovery bound, independent of queue depth, without starving the
		// ordinary FIFO.
		const auto oneAtATimeValue = (overflowValue + 41) & 0x7f;
		hostWrite(probe, oneAtATimeValue);
		const auto beforeOneAtATime = controller.getTransmittedAutomationChangeCount();
		const auto maximumPasses = controller.getExposedParameters().size() * 2;
		size_t oneAtATimePasses = 0;
		while(controller.getTransmittedAutomationChangeCount() == beforeOneAtATime
			&& oneAtATimePasses < maximumPasses)
		{
			controller.processRealtimeParameterChanges(1);
			++oneAtATimePasses;
		}
		require(controller.getTransmittedAutomationChangeCount()
			== beforeOneAtATime + 1,
			"minimum-budget overflow recovery exceeded two slot-table traversals");
	}

	std::vector<uint8_t> withoutChunk(const std::vector<uint8_t>& _state,
		const char* const _tag)
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
			const auto matches = std::memcmp(
				_state.data() + position, _tag, 4) == 0;
			if(matches)
				removed = true;
			else
				result.insert(result.end(), _state.begin() + position,
					_state.begin() + position + chunkSize);
			position += chunkSize;
		}
		require(removed, std::string("saved custom state did not contain ")
			+ std::string(_tag, 4) + " chunk");
		return result;
	}

	std::vector<uint8_t> withoutAutomationChunk(const std::vector<uint8_t>& _state)
	{
		return withoutChunk(_state, "AUTO");
	}

	void verifyRamRecordingModeState()
	{
		Harness mdHarness(md::MachineModel::Machinedrum);
		auto& processor = mdHarness.processor;
		require(processor.getRamRecordingMode() == md::RamRecordingMode::CompleteTail,
			"new Machinedrum instance did not default to complete RAM tails");

		processor.setRamRecordingMode(md::RamRecordingMode::Original);
		std::vector<uint8_t> originalState;
		processor.saveCustomData(originalState);
		processor.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
		require(processor.loadCustomData(originalState),
			"RAM recording mode state was rejected");
		require(processor.getRamRecordingMode() == md::RamRecordingMode::Original,
			"saved original RAM recording mode was not restored");

		processor.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
		std::vector<uint8_t> completeState;
		processor.saveCustomData(completeState);
		const auto legacyState = withoutChunk(completeState, "RAMF");
		processor.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
		require(processor.loadCustomData(legacyState),
			"legacy state without RAMF was rejected");
		require(processor.getRamRecordingMode() == md::RamRecordingMode::Original,
			"legacy state did not preserve original RAM recording behavior");

		Harness mmHarness(md::MachineModel::Monomachine);
		require(mmHarness.processor.getRamRecordingMode() == md::RamRecordingMode::Original,
			"Monomachine instance exposed a Machinedrum RAM mode");
		mmHarness.processor.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
		require(mmHarness.processor.getRamRecordingMode() == md::RamRecordingMode::Original,
			"Monomachine accepted a Machinedrum RAM mode");
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
		const auto headerSize = baseline[0] == 1 ? size_t{6} : size_t{7};
		const auto entrySize = baseline[0] == 1 ? size_t{4} : size_t{5};
		auto invalid = baseline;
		invalid.push_back(0);
		expectInvalid(invalid, "trailing byte");
		invalid = baseline;
		invalid[0] = 3;
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
		invalid[headerSize] = 0xff;
		expectInvalid(invalid, "unknown address");
		invalid = baseline;
		invalid[headerSize + entrySize] = invalid[headerSize];
		invalid[headerSize + entrySize + 1] = invalid[headerSize + 1];
		invalid[headerSize + entrySize + 2] = invalid[headerSize + 2];
		expectInvalid(invalid, "duplicate address");
		invalid = baseline;
		invalid[headerSize + 3] = 0xff;
		expectInvalid(invalid, "out-of-range value");

		require(controller.restoreAutomationSnapshot(baseline),
			"valid snapshot was rejected");
		const auto pendingRestore = controller.createAutomationSnapshot();
		require(!pendingRestore.empty() && !snapshotIsComplete(pendingRestore),
			"restoring a snapshot did not retain pending intent while resynchronizing");
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

	void verifyPendingStateBeforeSynchronization(const md::MachineModel _model)
	{
		Harness source(_model);
		auto& parameter = *parameters(source, false).front();
		const auto value = static_cast<int>(
			(parameter.getUnnormalizedValue() + 47) & 0x7f);
		const float oldUiValue = parameter.getValueObject().getValue();
		realtimeAllocationProbe::allocations = 0;
		realtimeAllocationProbe::enabled = true;
		hostWrite(parameter, value);
		realtimeAllocationProbe::enabled = false;
		require(realtimeAllocationProbe::allocations == 0,
			"audio-thread host write performed a heap allocation");
		require(parameter.getUnnormalizedValue() == value,
			"host-facing value was not atomically visible before synchronization");
		require(static_cast<float>(parameter.getValueObject().getValue()) == oldUiValue,
			"audio-thread host write synchronously touched juce::Value");

		const auto pending = source.controller.createAutomationSnapshot();
		require(!pending.empty() && !snapshotIsComplete(pending)
			&& snapshotValue(pending, parameter) == value,
			"pre-synchronization host intent was not serializable");

		Harness restored(_model);
		mdJucePlugin::ControllerAutomationTestAccess::useSyntheticFirmware(
			restored.controller);
		primeSyntheticSnapshot(restored, 41);
		auto restoredParameters = parameters(restored, false);
		require(restoredParameters.size() >= 2,
			"partial restore baseline test needs two parameters");
		auto& restoredParameter = *restoredParameters[0];
		auto& restoredBaselineParameter = *restoredParameters[1];
		require(restored.controller.restoreAutomationSnapshot(pending),
			"pending-only automation snapshot was rejected");
		const auto roundTrip = restored.controller.createAutomationSnapshot();
		require(!roundTrip.empty() && !snapshotIsComplete(roundTrip)
			&& snapshotValue(roundTrip, restoredParameter) == value,
			"pending-only automation intent did not survive restore");
		const auto firmwareBaseline = static_cast<uint8_t>(value == 23 ? 24 : 23);
		primeSyntheticSnapshot(restored, firmwareBaseline);
		require(restoredParameter.getUnnormalizedValue() == value,
			"firmware baseline overwrote partial-state host intent");
		require(restoredBaselineParameter.getUnnormalizedValue() == firmwareBaseline,
			"partial restore retained an unrelated previous-session value");

		source.controller.processPendingMidiMessages();
		require(juce::roundToInt(static_cast<float>(
			parameter.getValueObject().getValue())) == value,
			"message-thread service did not flush the atomic value to the editor mirror");
	}

	void verifyStateLoadReplacesSameSlotBaseline(Harness& _harness)
	{
		auto& controller = _harness.controller;
		auto& probe = *parameters(_harness, false).front();
		require(probe.getUnnormalizedValue() == 23,
			"same-slot state-load test has no initial baseline");

		// onStateLoaded represents replacement of the serialized device, not a
		// periodic refresh. The replacement may select the same Kit number while
		// containing different bytes (notably legacy states without an AUTO chunk).
		controller.onStateLoaded();
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Global, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(statusResponse(_harness.model,
			md::automation::sysex::StatusParameter::Kit, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeGlobalDump(_harness.model, 0, 0),
			synthLib::MidiEventSource::Device);
		controller.parseSysexMessage(makeKitDump(_harness.model, 0, 41),
			synthLib::MidiEventSource::Device);
		require(controller.isAutomationSynchronized()
			&& probe.getUnnormalizedValue() == 41,
			"state load retained the previous session's same-slot baseline");
	}

	void verifyMuteOwnership(Harness& _harness)
	{
		const auto mutePage = _harness.model == md::MachineModel::Monomachine
			? md::automation::monomachine::Mute
			: md::automation::machinedrum::Mute;
		pluginLib::Parameter* mute = nullptr;
		for(auto* const parameter : parameters(_harness, true))
		{
			if(parameter->getDescription().page == mutePage)
			{
				mute = parameter;
				break;
			}
		}
		require(mute != nullptr, "mute automation parameter is missing");
		const auto desired = mute->getUnnormalizedValue() == 0 ? 1 : 0;
		hostWrite(*mute, desired);

		// Mute is live/session state and is absent from Elektron's Kit dump format.
		// A later authoritative Kit refresh must therefore retain host intent rather
		// than inventing a value from unrelated/default data.
		primeSyntheticSnapshot(_harness);
		require(mute->getUnnormalizedValue() == desired,
			"Kit synchronization overwrote session-owned mute state");
		const auto snapshot = _harness.controller.createAutomationSnapshot();
		require(snapshotIsComplete(snapshot)
			&& snapshotValue(snapshot, *mute) == desired,
			"session-owned mute state was not persisted in project state");
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

	bool verifyModel(const md::MachineModel _model)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
			return allowMissingFirmware("mdAutomationRobustnessTest", _model);
		verifyQueuedPreBootWrites(harness);
		verifyExternalNotifications(harness);
		verifyCorrelatedDumpsAndStrictStatus(harness);
		verifyMidiNoneReplay(harness);
		verifyOrderedIntentArchitecture(harness);
		verifyStateContract(harness);
		verifyRandomizedLifecycle(harness);
		std::cout << "mdAutomationRobustnessTest: " << modelName(_model)
			<< " PASS\n";
		return true;
	}

	void verifyArchitecture(const md::MachineModel _model)
	{
		verifyRetriedKitSynchronization(_model, false);
		verifyRetriedKitSynchronization(_model, true);
		verifyPeriodicControllerPolling(_model);
		if(_model == md::MachineModel::Machinedrum)
			verifyRamRecordingModeState();
		verifyPendingStateBeforeSynchronization(_model);
		Harness harness(_model);
		mdJucePlugin::ControllerAutomationTestAccess::useSyntheticFirmware(
			harness.controller);
		primeSyntheticSnapshot(harness);
		verifyStateLoadReplacesSameSlotBaseline(harness);
		verifyMuteOwnership(harness);
		verifyOrderedIntentArchitecture(harness);
		verifyAdversarialRestoreSynchronization(harness);
		verifyConcurrentPublicationArchitecture(harness);
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
		bool ranFirmwareCase = false;
		if(!mmOnly)
			ranFirmwareCase = verifyModel(md::MachineModel::Machinedrum)
				|| ranFirmwareCase;
		if(!mdOnly)
			ranFirmwareCase = verifyModel(md::MachineModel::Monomachine)
				|| ranFirmwareCase;
		return ranFirmwareCase ? 0 : mdAutomationTest::SkipReturnCode;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdAutomationRobustnessTest: " << error.what() << '\n';
		return 1;
	}
}
