#include "mdPluginProcessor.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdmemorymap.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdstate.h"
#include "mdLib/mdtypes.h"

#include "baseLib/binarystream.h"
#include "juce_audio_utils/juce_audio_utils.h"
#include "synthLib/romLoader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
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
		static FrontPanelPublisher* publisher(Hardware& _hardware)
		{
			return _hardware.m_frontPanelPublisher.get();
		}
		static FrontPanelPublisher* livePublisher(Device& _device)
		{
			return _device.m_frontPanelPublisher.get();
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

	std::vector<uint8_t> mdPluginState(const std::vector<uint8_t>& _patch,
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

	std::vector<uint8_t> mmPluginState(const std::vector<uint8_t>& _patch,
		const std::vector<uint8_t>& _userFlash = {})
	{
		std::vector<uint8_t> state;
		const auto encoded = _userFlash.empty()
			? md::encodeState(state, _patch, md::MachineModel::Monomachine,
				synthLib::StateTypeGlobal)
			: md::encodeState(state, _patch, md::MachineModel::Monomachine,
				synthLib::StateTypeGlobal, _userFlash);
		require(encoded,
			"could not encode Monomachine processor restore fixture");
		std::vector<uint8_t> result{1, synthLib::StateTypeGlobal};
		result.insert(result.end(), state.begin(), state.end());
		return result;
	}

	std::vector<uint8_t> mdCompletePluginState(const std::vector<uint8_t>& _patch,
		const std::vector<uint8_t>& _flash, const std::vector<uint8_t>& _rom)
	{
		std::vector<uint8_t> state;
		require(md::encodeState(state, _patch, _flash, _rom, _rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"could not encode complete Machinedrum cold-start fixture");
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
		uint64_t hardwareEpoch = 0;
		bool candidateReady = false;
	};

	class Harness
	{
	public:
		explicit Harness(const md::MachineModel _model = md::MachineModel::Machinedrum,
			const bool _prepareAudio = true)
			: processor(_model, isolatedConfig(), false)
			, audioProcessor(processor), audio(2, blockSize)
		{
			if(_prepareAudio)
				prepareAudio();
		}

		~Harness()
		{
			if(audioPrepared)
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
			require(audioPrepared, "audio callback ran before processor preparation");
			for(uint32_t block = 0; block < _blocks; ++block)
			{
				audio.clear();
				midi.clear();
				audioProcessor.processBlock(audio, midi);
				if((block & 31u) == 0)
					std::this_thread::sleep_for(250us);
			}
		}

		void prepareAudio()
		{
			require(!audioPrepared, "processor audio was prepared twice");
			audioProcessor.prepareToPlay(48000.0, blockSize);
			audioPrepared = true;
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
					require(device != nullptr, "processor did not own a local MD/MM device");
					result.status = device->projectStateRestoreStatus();
					result.generation = device->deferredStateGeneration();
					result.liveHardware = &device->getHardware();
					result.hardwareEpoch = device->hardwareEpoch();
					auto* const candidate =
						md::DevicePreparedStateTestAccess::deferredHardware(*device);
					result.candidateReady = candidate
						&& !candidate->isProjectStateRestorePending();
					return result;
				});
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
		bool audioPrepared = false;
	};

	void setHostState(Harness& _harness, const std::vector<uint8_t>& _state)
	{
		const auto hostState = processorState(_state);
		_harness.audioProcessor.setStateInformation(hostState.data(),
			static_cast<int>(hostState.size()));
	}

	class HeadlessLifecycleAudioDevice final : public juce::AudioIODevice
	{
	public:
		HeadlessLifecycleAudioDevice() : AudioIODevice("Lifecycle device", "Test") {}
		void configure(const double _sampleRate, const int _bufferSize,
			const int _inputs, const int _outputs)
		{
			sampleRate = _sampleRate;
			bufferSize = _bufferSize;
			inputs = _inputs;
			outputs = _outputs;
		}
		juce::StringArray getOutputChannelNames() override { return {"L", "R"}; }
		juce::StringArray getInputChannelNames() override { return {"L", "R"}; }
		juce::Array<double> getAvailableSampleRates() override { return {sampleRate}; }
		juce::Array<int> getAvailableBufferSizes() override { return {bufferSize}; }
		int getDefaultBufferSize() override { return bufferSize; }
		juce::String open(const juce::BigInteger&, const juce::BigInteger&,
			double, int) override { return {}; }
		void close() override {}
		bool isOpen() override { return true; }
		void start(juce::AudioIODeviceCallback*) override {}
		void stop() override {}
		bool isPlaying() override { return true; }
		juce::String getLastError() override { return {}; }
		int getCurrentBufferSizeSamples() override { return bufferSize; }
		double getCurrentSampleRate() override { return sampleRate; }
		int getCurrentBitDepth() override { return 32; }
		juce::BigInteger getActiveOutputChannels() const override
		{
			juce::BigInteger result;
			result.setRange(0, outputs, true);
			return result;
		}
		juce::BigInteger getActiveInputChannels() const override
		{
			juce::BigInteger result;
			result.setRange(0, inputs, true);
			return result;
		}
		int getOutputLatencyInSamples() override { return 0; }
		int getInputLatencyInSamples() override { return 0; }

	private:
		double sampleRate = 48000.0;
		int bufferSize = 128;
		int inputs = 2;
		int outputs = 2;
	};

	void verifyHeadlessStandaloneLifecycle(const md::MachineModel _model,
		const std::vector<uint8_t>& _state)
	{
		Harness harness(_model, false);
		const auto before = harness.snapshot();
		setHostState(harness, _state);
		const auto restored = harness.snapshot();
		require(restored.status == RestoreStatus::Idle
			&& restored.hardwareEpoch == before.hardwareEpoch + 1
			&& restored.liveHardware != before.liveHardware,
			"standalone-order cold restore did not commit before audio startup");

		HeadlessLifecycleAudioDevice device;
		juce::AudioProcessorPlayer player;
		player.audioDeviceAboutToStart(&device);
		player.setProcessor(&harness.processor);
		std::array<float, 128> input{};
		std::array<float, 128> output{};
		input.fill(0.125f);
		const float* inputs[] = {nullptr, input.data()};
		float* outputs[] = {output.data(), nullptr};
		for(uint32_t block = 0; block < 32; ++block)
			player.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 2, 128, {});
		require(std::all_of(output.begin(), output.end(),
			[](const float value) { return std::isfinite(value); }),
			"standalone-order sparse callback produced invalid audio");

		player.audioDeviceStopped();
		device.configure(44100.0, 512, 1, 2);
		player.audioDeviceAboutToStart(&device);
		std::array<float, 512> bluetoothInput{};
		std::array<float, 512> bluetoothOutput{};
		const float* bluetoothInputs[] = {bluetoothInput.data()};
		float* bluetoothOutputs[] = {bluetoothOutput.data(), nullptr};
		player.audioDeviceIOCallbackWithContext(bluetoothInputs, 1,
			bluetoothOutputs, 2, 512, {});
		require(std::all_of(bluetoothOutput.begin(), bluetoothOutput.end(),
			[](const float value) { return std::isfinite(value); }),
			"standalone-order device-change callback produced invalid audio");
		player.setProcessor(nullptr);
		player.audioDeviceStopped();
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
	const auto* const mmFirmwarePath = std::getenv("GEARMULATOR_MM_FIRMWARE_BIN");
	if(!firmwarePath || !*firmwarePath || !mmFirmwarePath || !*mmFirmwarePath)
	{
		std::cout << "mdProjectStateRestoreTest: SKIP (pinned MD/MM firmware not supplied)\n";
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
		std::ifstream mmInput(mmFirmwarePath, std::ios::binary);
		const std::vector<uint8_t> mmRom{
			std::istreambuf_iterator<char>(mmInput), std::istreambuf_iterator<char>()};
		require(md::RomLoader::isRomForModel(mmRom, md::MachineModel::Monomachine),
			"firmware is not the supported Monomachine OS 1.32b image");
		synthLib::RomLoader::addSearchPath(
			juce::File(firmwarePath).getParentDirectory().getFullPathName().toStdString());
		synthLib::RomLoader::addSearchPath(
			juce::File(mmFirmwarePath).getParentDirectory().getFullPathName().toStdString());

		// Hardware contains the complete emulated machine and is larger than the
		// default 1 MiB Windows process stack. Keep it on the heap so both the
		// fixture-free skip and the real-firmware test can enter main safely.
		auto factory = std::make_unique<md::Hardware>(
			rom, firmwarePath, md::MachineModel::Machinedrum);
		require(factory->isValid() && initializeUwFlash(*factory),
			"could not establish the OS 1.63 factory flash baseline");
		const auto factoryFlash = factory->copyFlashData();
		const auto factoryPatch = factory->getUC().copyPatchRam();

		auto flashA = factoryFlash;
		auto patchA = factoryPatch;
		flashA[6 * md::g_uwFlashSectorSize + 111] ^= 0x21;
		patchA.front() ^= 0x31;
		const auto stateA = mdPluginState(patchA, flashA, factoryFlash, rom);
		const auto completeStateA = mdCompletePluginState(patchA, flashA, rom);

		auto flashB = factoryFlash;
		auto patchB = factoryPatch;
		flashB[10 * md::g_uwFlashSectorSize + 222] ^= 0x42;
		patchB.back() ^= 0x52;
		const auto stateB = mdPluginState(patchB, flashB, factoryFlash, rom);

		auto flashC = factoryFlash;
		auto patchC = factoryPatch;
		flashC[12 * md::g_uwFlashSectorSize + 333] ^= 0x63;
		patchC[patchC.size() / 2] ^= 0x73;
		const auto stateC = mdPluginState(patchC, flashC, factoryFlash, rom);

		verifyHeadlessStandaloneLifecycle(
			md::MachineModel::Machinedrum, completeStateA);

		Harness successful;
		successful.process(64);
		const auto liveMicros = successful.benchmark(128);
		setHostState(successful, stateA);
		const auto initial = successful.snapshot();
		require(initial.status == RestoreStatus::Initializing,
			"processor did not begin an isolated deferred restore");
		require(serializedPluginState(successful.processor) == stateA,
			"processor autosave lost the requested state during initialization");

		size_t liveMidiQueuedBefore = 0;
		size_t candidateMidiQueuedBefore = 0;
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
				require(md::DevicePreparedStateTestAccess::publisher(live)
					== md::DevicePreparedStateTestAccess::livePublisher(*device)
					&& md::DevicePreparedStateTestAccess::publisher(*candidate)
					!= md::DevicePreparedStateTestAccess::livePublisher(*device),
					"prepared and live machines shared the SPSC front-panel publisher");
				const auto livePanelBytes = live.getPendingPanelInputBytes();
				const auto candidatePanelBytes = candidate->getPendingPanelInputBytes();
				device->sendPanelEvent(0x24, 0x02); // MD Function press.
				require(live.getPendingPanelInputBytes() == livePanelBytes + 2
					&& candidate->getPendingPanelInputBytes() == candidatePanelBytes,
					"panel input was not routed exclusively to the audible live machine");
				liveMidiQueuedBefore = live.queuedMidiRxBytes();
				candidateMidiQueuedBefore = candidate->queuedMidiRxBytes();
			});
		successful.processor.getPlugin().addMidiEvent(synthLib::SMidiEvent(
			synthLib::MidiEventSource::Host, synthLib::M_CONTROLCHANGE, 16, 64));
		bool liveMidiQueued = false;
		bool liveMidiConsumed = false;
		bool candidateMidiUntouched = true;
		for(uint32_t block = 0; block < 24000 && !liveMidiConsumed; ++block)
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
					const auto liveMidiQueuedNow =
						device->getHardware().queuedMidiRxBytes();
					liveMidiQueued = liveMidiQueued
						|| liveMidiQueuedNow > liveMidiQueuedBefore;
					liveMidiConsumed = liveMidiQueued
						&& liveMidiQueuedNow == liveMidiQueuedBefore;
					candidateMidiUntouched = candidateMidiUntouched
						&& candidate->queuedMidiRxBytes() == candidateMidiQueuedBefore;
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
		std::atomic<bool> keepProcessing{true};
		std::atomic<bool> audioStarted{false};
		std::atomic<uint32_t> concurrentAudioBlocks{0};
		std::thread audioThread([&]
		{
			audioStarted.store(true, std::memory_order_release);
			while(keepProcessing.load(std::memory_order_acquire))
			{
				successful.process(1);
				concurrentAudioBlocks.fetch_add(1, std::memory_order_relaxed);
			}
		});
		while(!audioStarted.load(std::memory_order_acquire))
			std::this_thread::yield();
		gate.release();
		threadB.join();
		threadC.join();
		keepProcessing.store(false, std::memory_order_release);
		audioThread.join();
		require(concurrentAudioBlocks.load(std::memory_order_relaxed) > 0,
			"audio did not run during concurrent replacement construction");
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
		require(completed.liveHardware->getPendingPanelInputBytes() == 0,
			"ephemeral live-machine panel input was replayed into the replacement");
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
				require(md::DevicePreparedStateTestAccess::publisher(device->getHardware())
					== md::DevicePreparedStateTestAccess::livePublisher(*device),
					"committed machine was not rebound to the live front-panel publisher");
			});

		// A malformed cold-start payload must fail deterministically and identify the
		// layer that rejected it, rather than collapsing every failure into the same
		// generic standalone alert.
		auto malformed = stateA;
		malformed.resize(17);
		setHostState(successful, malformed);
		require(successful.snapshot().status == RestoreStatus::Failed,
			"malformed state did not enter the failed restore state");
		require(successful.processor.getProjectStateRestoreError().find("payload")
			!= std::string::npos,
			"malformed state failure did not retain an actionable diagnostic");

		auto wrongFactory = factoryFlash;
		wrongFactory[2 * md::g_uwFlashSectorSize + 17] ^= 0x41;
		auto wrongProject = wrongFactory;
		wrongProject[14 * md::g_uwFlashSectorSize + 91] ^= 0x24;
		const auto rejectedState = mdPluginState(factoryPatch, wrongProject,
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

		// MM does not need the MD factory-flash initialization phase, but it uses
		// the same unlocked replacement transaction. Exercise its real firmware on
		// the standalone ordering: restore first, then prepare/start audio.
		std::vector<uint8_t> mmPatchA(md::g_patchRamStateSize, 0);
		std::vector<uint8_t> mmPatchB(md::g_patchRamStateSize, 0);
		std::vector<uint8_t> mmPatchC(md::g_patchRamStateSize, 0);
		for(size_t offset = 0; offset < mmPatchA.size(); offset += 4093)
		{
			mmPatchA[offset] = static_cast<uint8_t>(offset >> 8);
			mmPatchB[offset] = static_cast<uint8_t>((offset >> 7) ^ 0x5a);
			mmPatchC[offset] = static_cast<uint8_t>((offset >> 6) ^ 0xa5);
		}
		const auto mmStateA = mmPluginState(mmPatchA);
		const auto mmStateB = mmPluginState(mmPatchB);
		const auto mmStateC = mmPluginState(mmPatchC);
		verifyHeadlessStandaloneLifecycle(md::MachineModel::Monomachine, mmStateA);

		Harness mmCold(md::MachineModel::Monomachine, false);
		const auto mmBefore = mmCold.snapshot();
		setHostState(mmCold, mmStateA);
		const auto mmRestoredBeforeAudio = mmCold.snapshot();
		require(mmRestoredBeforeAudio.status == RestoreStatus::Idle
			&& mmRestoredBeforeAudio.hardwareEpoch == mmBefore.hardwareEpoch + 1
			&& mmRestoredBeforeAudio.liveHardware != mmBefore.liveHardware,
			"Monomachine cold state was not committed before audio startup");
		// Version-1 projects contain only patch RAM. Saving them upgrades to the
		// current format, adding the ROM's DigiPRO flash window. Compare against
		// that complete expected state, including every patch and flash byte.
		const auto mmFlashBegin = mmRom.begin() + md::memorymap::g_flashFull.offset(
			md::memorymap::g_mmUserFlash.begin);
		const std::vector<uint8_t> mmDefaultFlash(mmFlashBegin,
			mmFlashBegin + md::g_mmUserFlashStateSize);
		require(serializedPluginState(mmCold.processor)
			== mmPluginState(mmPatchA, mmDefaultFlash),
			"Monomachine legacy restore changed patch RAM or default DigiPRO flash");
		require(mmRestoredBeforeAudio.liveHardware->copyPatchRam() == mmPatchA
			&& mmRestoredBeforeAudio.liveHardware->copyUserFlash() == mmDefaultFlash,
			"Monomachine legacy restore did not initialize the live machine");

		// Current projects must also restore their private flash, rather than
		// replacing it with ROM bytes during the cold-start hardware transaction.
		auto mmProjectFlash = mmDefaultFlash;
		mmProjectFlash.front() ^= 0x31;
		mmProjectFlash[mmProjectFlash.size() / 2] ^= 0x52;
		mmProjectFlash.back() ^= 0x73;
		const auto mmCompleteStateA = mmPluginState(mmPatchA, mmProjectFlash);
		setHostState(mmCold, mmCompleteStateA);
		const auto mmCompleteRestore = mmCold.snapshot();
		require(mmCompleteRestore.status == RestoreStatus::Idle
			&& mmCompleteRestore.hardwareEpoch == mmRestoredBeforeAudio.hardwareEpoch + 1
			&& mmCompleteRestore.liveHardware != mmRestoredBeforeAudio.liveHardware,
			"Monomachine complete state was not committed before audio startup");
		require(serializedPluginState(mmCold.processor) == mmCompleteStateA,
			"Monomachine cold-start serialization lost patch RAM or DigiPRO flash");
		require(mmCompleteRestore.liveHardware->copyPatchRam() == mmPatchA
			&& mmCompleteRestore.liveHardware->copyUserFlash() == mmProjectFlash,
			"Monomachine complete restore did not initialize the live machine");
		mmCold.prepareAudio();
		mmCold.process(128);

		StartGate mmGate;
		bool mmResultB = false;
		bool mmResultC = false;
		std::thread mmThreadB([&]
		{
			mmGate.arriveAndWait();
			mmResultB = mmCold.processor.getPlugin().setState(mmStateB);
		});
		std::thread mmThreadC([&]
		{
			mmGate.arriveAndWait();
			mmResultC = mmCold.processor.getPlugin().setState(mmStateC);
		});
		std::atomic<bool> mmKeepProcessing{true};
		std::atomic<uint32_t> mmAudioBlocks{0};
		std::thread mmAudioThread([&]
		{
			while(mmKeepProcessing.load(std::memory_order_acquire))
			{
				mmCold.process(1);
				mmAudioBlocks.fetch_add(1, std::memory_order_relaxed);
			}
		});
		mmGate.release();
		mmThreadB.join();
		mmThreadC.join();
		mmKeepProcessing.store(false, std::memory_order_release);
		mmAudioThread.join();
		require(mmAudioBlocks.load(std::memory_order_relaxed) > 0,
			"Monomachine audio did not run during replacement construction");
		require(mmResultB || mmResultC,
			"all concurrent Monomachine restore requests failed");
		const auto mmWinningState = serializedPluginState(mmCold.processor);
		require(mmWinningState.size() > 2 && mmWinningState[0] == 1
			&& mmWinningState[1] == synthLib::StateTypeGlobal,
			"concurrent Monomachine restore published an invalid device envelope");
		std::vector<uint8_t> mmWinningPayload(mmWinningState.begin() + 2,
			mmWinningState.end());
		md::DecodedState mmWinningDecoded;
		require(md::decodeState(mmWinningDecoded, mmWinningPayload, {},
			md::MachineModel::Monomachine, synthLib::StateTypeGlobal)
			&& mmWinningDecoded.containsFlash,
			"concurrent Monomachine restore published a torn or corrupt state");
		mmCold.processor.getPlugin().withDeviceLocked(
			[&](synthLib::Device* const _device)
			{
				auto* const device = dynamic_cast<md::Device*>(_device);
				require(device && device->getModel() == md::MachineModel::Monomachine
					&& device->projectStateRestoreStatus() == RestoreStatus::Idle
					&& device->getHardware().copyPatchRam() == mmWinningDecoded.patchRam
					&& device->getHardware().copyUserFlash() == mmWinningDecoded.userFlash,
					"Monomachine serialization did not match the committed live machine");
				require(md::DevicePreparedStateTestAccess::publisher(device->getHardware())
					== md::DevicePreparedStateTestAccess::livePublisher(*device),
					"committed Monomachine was not rebound to the live panel publisher");
			});

		auto malformedMm = mmStateA;
		malformedMm.resize(23);
		setHostState(mmCold, malformedMm);
		require(mmCold.snapshot().status == RestoreStatus::Failed
			&& mmCold.processor.getProjectStateRestoreError().find("Monomachine")
				!= std::string::npos,
			"malformed Monomachine state did not retain a model-specific diagnostic");

		std::cout << "mdProjectStateRestoreTest: MD/MM PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdProjectStateRestoreTest: " << error.what() << '\n';
		return 1;
	}
}
