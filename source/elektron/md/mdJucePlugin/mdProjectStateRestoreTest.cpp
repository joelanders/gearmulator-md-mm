#include "mdPluginProcessor.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/binarystream.h"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace md
{
	struct DevicePreparedStateTestAccess
	{
		static Hardware* deferredHardware(Device& _device)
		{
			return _device.m_deferredPreparedState
				? _device.m_deferredPreparedState->m_hardware.get() : nullptr;
		}
	};
}

namespace
{
	using RestoreStatus = md::Device::ProjectStateRestoreStatus;
	using namespace std::chrono_literals;

	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void advance(md::Hardware& _hardware, const uint32_t _frames)
	{
		constexpr uint32_t block = 128;
		for(uint32_t frames = 0; frames < _frames; frames += block)
			_hardware.advance(std::min(block, _frames - frames));
	}

	bool initializeUwFlash(md::Hardware& _hardware)
	{
		advance(_hardware, md::g_samplerate * 5);
		if(_hardware.isFactoryFlashReadyForReboot())
			return true;
		for(uint32_t instruction = 0; instruction < 200'000'000; ++instruction)
			_hardware.processUC();
		for(uint32_t instruction = 0; instruction < 100'000'000; ++instruction)
		{
			_hardware.processUC();
			if((instruction & 1023u) == 0)
			{
				_hardware.advance(0);
				if(_hardware.isFactoryFlashReadyForReboot())
					return true;
			}
		}
		return _hardware.isFactoryFlashReadyForReboot();
	}

	std::vector<uint8_t> pluginState(const std::vector<uint8_t>& _patch,
		const std::vector<uint8_t>& _flash,
		const std::vector<uint8_t>& _factory,
		const std::vector<uint8_t>& _rom)
	{
		std::vector<uint8_t> state;
		require(md::encodeStateWithFactoryBaseline(state, _patch, _flash, _factory,
			_rom, md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"could not encode processor restore fixture");
		std::vector<uint8_t> result{1, synthLib::StateTypeGlobal};
		result.insert(result.end(), state.begin(), state.end());
		return result;
	}

	std::vector<uint8_t> processorState(const std::vector<uint8_t>& _pluginState)
	{
		baseLib::BinaryStream chunks;
		{
			baseLib::ChunkWriter midi(chunks, "MIDI", 1);
			chunks.write(_pluginState);
		}
		std::vector<uint8_t> customData;
		chunks.toVector(customData);

		baseLib::BinaryStream outer;
		outer.write(std::string("DSP56300"));
		outer.write<uint32_t>(2);
		outer.write(customData);
		std::vector<uint8_t> result;
		outer.toVector(result);
		return result;
	}

	std::vector<uint8_t> serializedPluginState(juce::AudioProcessor& _processor)
	{
		juce::MemoryBlock block;
		_processor.getStateInformation(block);
		const auto* const begin = static_cast<const uint8_t*>(block.getData());
		std::vector<uint8_t> bytes(begin, begin + block.getSize());
		baseLib::BinaryStream outer(bytes);
		require(outer.readString() == "DSP56300", "processor wrote the wrong state magic");
		require(outer.read<uint32_t>() <= 2, "processor wrote an unknown state version");
		std::vector<uint8_t> customData;
		outer.read(customData);

		baseLib::BinaryStream chunks(customData);
		baseLib::ChunkReader reader(chunks);
		std::vector<uint8_t> result;
		reader.add("MIDI", 1, [&](baseLib::BinaryStream& _chunk, uint32_t)
		{
			_chunk.read(result);
		});
		require(reader.tryRead() && !result.empty(),
			"processor state did not contain a readable MIDI/device chunk");
		return result;
	}

	class HostListener final : public juce::AudioProcessorListener
	{
	public:
		void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
		void audioProcessorChanged(juce::AudioProcessor*,
			const ChangeDetails& _details) override
		{
			if(_details.nonParameterStateChanged)
				++nonParameterChanges;
		}

		std::atomic<uint32_t> nonParameterChanges{0};
	};

	struct RestoreSnapshot
	{
		RestoreStatus status = RestoreStatus::Failed;
		uint64_t generation = 0;
		md::Hardware* liveHardware = nullptr;
		bool candidateReady = false;
	};

	class Harness
	{
	public:
		Harness()
			: processor(md::MachineModel::Machinedrum, isolatedConfig(), false)
			, audioProcessor(processor), audio(2, blockSize)
		{
			audioProcessor.prepareToPlay(48000.0, blockSize);
		}

		~Harness()
		{
			audioProcessor.releaseResources();
		}

		static mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig isolatedConfig()
		{
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig result;
			result.deviceHomePath = std::string{};
			return result;
		}

		void process(const uint32_t _blocks)
		{
			for(uint32_t block = 0; block < _blocks; ++block)
			{
				audio.clear();
				midi.clear();
				audioProcessor.processBlock(audio, midi);
				if((block & 31u) == 0)
					std::this_thread::sleep_for(250us);
			}
		}

		double benchmark(const uint32_t _blocks)
		{
			const auto begin = std::chrono::steady_clock::now();
			process(_blocks);
			return std::chrono::duration<double, std::micro>(
				std::chrono::steady_clock::now() - begin).count()
				/ static_cast<double>(_blocks);
		}

		RestoreSnapshot snapshot()
		{
			return processor.getPlugin().withDeviceLocked(
				[](synthLib::Device* const _device)
				{
					RestoreSnapshot result;
					auto* const device = dynamic_cast<md::Device*>(_device);
					require(device != nullptr, "processor did not own a local MD device");
					result.status = device->projectStateRestoreStatus();
					result.generation = device->deferredStateGeneration();
					result.liveHardware = &device->getHardware();
					auto* const candidate =
						md::DevicePreparedStateTestAccess::deferredHardware(*device);
					result.candidateReady = candidate
						&& !candidate->isProjectStateRestorePending();
					return result;
				});
		}

		bool runUntilLiveMidiReady(const uint32_t _maximumBlocks = 24000)
		{
			for(uint32_t block = 0; block < _maximumBlocks; ++block)
			{
				process(1);
				if(processor.getPlugin().withDeviceLocked(
					[](synthLib::Device* const _device)
					{
						auto* const device = dynamic_cast<md::Device*>(_device);
						return device && device->getHardware().isFirmwareMidiReady();
					}))
					return true;
			}
			return false;
		}

		bool runUntilCandidateReady(const uint32_t _maximumBlocks = 24000)
		{
			for(uint32_t block = 0; block < _maximumBlocks; ++block)
			{
				process(1);
				const auto state = snapshot();
				if(state.candidateReady)
					return true;
				if(state.status == RestoreStatus::Failed
					|| state.status == RestoreStatus::Idle)
					return false;
			}
			return false;
		}

		mdJucePlugin::AudioPluginAudioProcessor processor;
		juce::AudioProcessor& audioProcessor;

	private:
		static constexpr int blockSize = 128;
		juce::AudioBuffer<float> audio;
		juce::MidiBuffer midi;
	};

	void setHostState(Harness& _harness, const std::vector<uint8_t>& _state)
	{
		const auto hostState = processorState(_state);
		_harness.audioProcessor.setStateInformation(hostState.data(),
			static_cast<int>(hostState.size()));
	}

	struct StartGate
	{
		void arriveAndWait()
		{
			std::unique_lock lock(mutex);
			if(++arrived == 2)
				condition.notify_all();
			condition.wait(lock, [&] { return go; });
		}

		void release()
		{
			std::unique_lock lock(mutex);
			condition.wait(lock, [&] { return arrived == 2; });
			go = true;
			lock.unlock();
			condition.notify_all();
		}

		std::mutex mutex;
		std::condition_variable condition;
		uint32_t arrived = 0;
		bool go = false;
	};
}

int main()
{
	const auto* const firmwarePath = std::getenv("GEARMULATOR_MD_FIRMWARE_BIN");
	if(!firmwarePath || !*firmwarePath)
	{
		std::cout << "mdProjectStateRestoreTest: SKIP (pinned MD firmware not supplied)\n";
		return 77;
	}

	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		std::ifstream input(firmwarePath, std::ios::binary);
		const std::vector<uint8_t> rom{
			std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		require(md::RomLoader::isRomForModel(rom, md::MachineModel::Machinedrum),
			"firmware is not the supported Machinedrum OS 1.63 image");
		synthLib::RomLoader::addSearchPath(
			juce::File(firmwarePath).getParentDirectory().getFullPathName().toStdString());

		md::Hardware factory(rom, firmwarePath, md::MachineModel::Machinedrum);
		require(factory.isValid() && initializeUwFlash(factory),
			"could not establish the OS 1.63 factory flash baseline");
		const auto factoryFlash = factory.copyFlashData();
		const auto factoryPatch = factory.getUC().copyPatchRam();

		auto flashA = factoryFlash;
		auto patchA = factoryPatch;
		flashA[6 * md::g_uwFlashSectorSize + 111] ^= 0x21;
		patchA.front() ^= 0x31;
		const auto stateA = pluginState(patchA, flashA, factoryFlash, rom);

		auto flashB = factoryFlash;
		auto patchB = factoryPatch;
		flashB[10 * md::g_uwFlashSectorSize + 222] ^= 0x42;
		patchB.back() ^= 0x52;
		const auto stateB = pluginState(patchB, flashB, factoryFlash, rom);

		auto flashC = factoryFlash;
		auto patchC = factoryPatch;
		flashC[12 * md::g_uwFlashSectorSize + 333] ^= 0x63;
		patchC[patchC.size() / 2] ^= 0x73;
		const auto stateC = pluginState(patchC, flashC, factoryFlash, rom);

		Harness successful;
		require(successful.runUntilLiveMidiReady(),
			"live OS 1.63 machine did not become MIDI-ready");
		const auto liveMicros = successful.benchmark(128);
		setHostState(successful, stateA);
		const auto initial = successful.snapshot();
		require(initial.status == RestoreStatus::Initializing,
			"processor did not begin an isolated deferred restore");
		require(serializedPluginState(successful.processor) == stateA,
			"processor autosave lost the requested state during initialization");

		uint64_t liveMidiConsumedBefore = 0;
		uint64_t candidateMidiConsumedBefore = 0;
		successful.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				require(device != nullptr, "pending restore lost its live MD device");
				auto& live = device->getHardware();
				auto* const candidate =
					md::DevicePreparedStateTestAccess::deferredHardware(*device);
				require(candidate != nullptr,
					"pending restore did not retain an isolated candidate");
				const auto livePanelBytes = live.getPendingPanelInputBytes();
				const auto candidatePanelBytes = candidate->getPendingPanelInputBytes();
				device->sendPanelEvent(0x24, 0x02); // MD Function press.
				require(live.getPendingPanelInputBytes() == livePanelBytes + 2
					&& candidate->getPendingPanelInputBytes() == candidatePanelBytes,
					"panel input was not routed exclusively to the audible live machine");
				liveMidiConsumedBefore = live.midiRxConsumedCount();
				candidateMidiConsumedBefore = candidate->midiRxConsumedCount();
			});
		successful.processor.getPlugin().addMidiEvent(synthLib::SMidiEvent(
			synthLib::MidiEventSource::Host, synthLib::M_CONTROLCHANGE, 16, 64));
		bool liveMidiConsumed = false;
		bool candidateMidiUntouched = true;
		for(uint32_t block = 0; block < 256 && !liveMidiConsumed; ++block)
		{
			successful.process(1);
			successful.processor.getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					auto* const candidate = device
						? md::DevicePreparedStateTestAccess::deferredHardware(*device) : nullptr;
					if(!device || !candidate)
					{
						candidateMidiUntouched = false;
						return;
					}
					liveMidiConsumed = device->getHardware().midiRxConsumedCount()
						> liveMidiConsumedBefore;
					candidateMidiUntouched = candidateMidiUntouched
						&& candidate->midiRxConsumedCount() == candidateMidiConsumedBefore;
				});
		}
		require(liveMidiConsumed && candidateMidiUntouched,
			"host MIDI was not consumed exclusively by the audible live machine");

		StartGate gate;
		bool resultB = false;
		bool resultC = false;
		std::thread threadB([&]
		{
			gate.arriveAndWait();
			resultB = successful.processor.getPlugin().setState(stateB);
		});
		std::thread threadC([&]
		{
			gate.arriveAndWait();
			resultC = successful.processor.getPlugin().setState(stateC);
		});
		gate.release();
		threadB.join();
		threadC.join();
		require(resultB || resultC, "all concurrent processor restore requests failed");

		std::vector<uint8_t> currentState;
		require(successful.processor.getPlugin().getState(
			currentState, synthLib::StateTypeGlobal),
			"processor could not serialize the winning concurrent restore");
		const bool stateBWon = currentState == stateB;
		require(stateBWon || currentState == stateC,
			"concurrent restore published a torn or stale requested state");
		const auto& expectedFlash = stateBWon ? flashB : flashC;
		const auto& expectedPatch = stateBWon ? patchB : patchC;
		const auto interrupted = successful.snapshot();
		require(interrupted.status == RestoreStatus::Initializing
			&& interrupted.generation >= initial.generation + 2
			&& interrupted.liveHardware == initial.liveHardware,
			"concurrent interruption mutated the audible live machine");
		require(serializedPluginState(successful.processor) == currentState,
			"processor autosave did not follow the winning restore generation");

		successful.process(64);
		const auto pendingMicros = successful.benchmark(128);
		const auto callbackCost = pendingMicros / liveMicros;
		std::cout << "Processor restore callback benchmark: live " << liveMicros
			<< " us/block, live+candidate " << pendingMicros << " us/block ("
			<< callbackCost << "x)\n";
		require(callbackCost <= 4.0,
			"processor restore callback cost exceeded the 4x safety bound");
		require(successful.runUntilCandidateReady(),
			"processor candidate did not finish OS 1.63 initialization");
		require(serializedPluginState(successful.processor) == currentState,
			"processor autosave changed before deferred completion");

		HostListener completionListener;
		successful.processor.addListener(&completionListener);
		require(successful.processor.serviceProjectStateRestore(),
			"processor did not service the completed deferred restore");
		const auto completed = successful.snapshot();
		require(completed.status == RestoreStatus::Idle
			&& completed.liveHardware != initial.liveHardware,
			"processor did not atomically publish the validated replacement");
		require(completed.liveHardware->getPendingPanelInputBytes() == 0
			&& completed.liveHardware->midiRxConsumedCount() == 0,
			"ephemeral live-machine interaction was replayed into the replacement");
		require(completionListener.nonParameterChanges.load() > 0,
			"processor did not notify the host after deferred completion");
		successful.processor.removeListener(&completionListener);
		require(serializedPluginState(successful.processor) == currentState,
			"processor serialization changed across deferred completion");
		successful.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				require(device && device->getHardware().copyFlashData() == expectedFlash
					&& device->getHardware().getUC().copyPatchRam() == expectedPatch,
					"processor committed the wrong concurrent restore generation");
			});

		auto wrongFactory = factoryFlash;
		wrongFactory[2 * md::g_uwFlashSectorSize + 17] ^= 0x41;
		auto wrongProject = wrongFactory;
		wrongProject[14 * md::g_uwFlashSectorSize + 91] ^= 0x24;
		const auto rejectedState = pluginState(factoryPatch, wrongProject,
			wrongFactory, rom);
		Harness rejected;
		setHostState(rejected, rejectedState);
		const auto rejectionStarted = rejected.snapshot();
		require(rejectionStarted.status == RestoreStatus::Initializing
			&& serializedPluginState(rejected.processor) == rejectedState,
			"processor did not retain the asynchronously validated state");
		require(rejected.runUntilCandidateReady(),
			"rejection candidate did not finish factory initialization");
		HostListener rejectionListener;
		rejected.processor.addListener(&rejectionListener);
		require(rejected.processor.serviceProjectStateRestore(),
			"processor did not surface the delayed restore rejection");
		const auto failed = rejected.snapshot();
		require(failed.status == RestoreStatus::Failed
			&& failed.liveHardware == rejectionStarted.liveHardware,
			"delayed rejection replaced the healthy live machine");
		require(!rejected.processor.getProjectStateRestoreError().empty(),
			"processor discarded the delayed restore error");
		require(rejectionListener.nonParameterChanges.load() > 0,
			"processor did not notify the host about delayed restore failure");
		require(!rejected.processor.serviceProjectStateRestore(),
			"processor reported the same restore failure more than once");
		rejected.processor.removeListener(&rejectionListener);
		require(serializedPluginState(rejected.processor) != rejectedState,
			"processor kept advertising a rejected project as authoritative");

		std::cout << "mdProjectStateRestoreTest: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdProjectStateRestoreTest: " << error.what() << '\n';
		return 1;
	}
}
