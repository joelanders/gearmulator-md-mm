#include "mdPluginProcessor.h"

#include "juce_audio_utils/juce_audio_utils.h"
#include "juce_events/juce_events.h"
#include "jucePluginLib/controller.h"
#include "jucePluginLib/processor.h"
#include "jucePluginLib/tools.h"
#include "baseLib/filesystem.h"
#include "synthLib/device.h"
#include "synthLib/plugin.h"
#include "synthLib/syntheticAudioTestDevice.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	void require(const bool _condition, const std::string& _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	using SyntheticAudioDevice = synthLib::test::SyntheticAudioDevice;

	void setDataRootEnvironment(const char* const _value)
	{
#if defined(_WIN32)
		_putenv_s("GEARMULATOR_DATA_ROOT", _value ? _value : "");
#else
		if(_value)
			setenv("GEARMULATOR_DATA_ROOT", _value, 1);
		else
			unsetenv("GEARMULATOR_DATA_ROOT");
#endif
	}

	void verifyIsolatedDataRoot()
	{
		const auto* const previousValue = std::getenv("GEARMULATOR_DATA_ROOT");
		const std::string previous = previousValue ? previousValue : "";
		const bool previouslySet = previousValue != nullptr;
		setDataRootEnvironment("/tmp/gearmulator-data-root-test");
		const auto actual = pluginLib::Tools::getPublicDataFolder(
			"Test Vendor", "Test Product");
		const auto expected = baseLib::filesystem::validatePath(
			"/tmp/gearmulator-data-root-test/Test Vendor/Test Product/");
		require(actual == expected,
			"release-test data root was ignored");
		setDataRootEnvironment(previouslySet ? previous.c_str() : nullptr);
	}

	class SparseCallbackAudioDevice final : public juce::AudioIODevice
	{
	public:
		SparseCallbackAudioDevice() : AudioIODevice("Sparse test device", "Test") {}
		void configure(const double _sampleRate, const int _bufferSize,
			const int _inputs, const int _outputs)
		{
			sampleRate = _sampleRate;
			bufferSize = _bufferSize;
			inputs = _inputs;
			outputs = _outputs;
		}

		juce::StringArray getOutputChannelNames() override { return {"Out 1", "Out 2"}; }
		juce::StringArray getInputChannelNames() override { return {"In 1", "In 2"}; }
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
		int bufferSize = 64;
		int inputs = 2;
		int outputs = 2;
	};

	class SyntheticController final : public pluginLib::Controller
	{
	public:
		explicit SyntheticController(pluginLib::Processor& _processor)
			: Controller(_processor)
		{
		}

		void sendParameterChange(const pluginLib::Parameter&, pluginLib::ParamValue,
			pluginLib::Parameter::Origin) override
		{
		}
		bool parseSysexMessage(const pluginLib::SysEx&,
			synthLib::MidiEventSource) override
		{
			return false;
		}
		void onStateLoaded() override {}
	};

	class SyntheticProcessor final : public pluginLib::Processor
	{
	public:
		SyntheticProcessor()
			: Processor(BusesProperties()
				.withInput("Input A/B", juce::AudioChannelSet::stereo(), true)
				.withOutput("Main A/B", juce::AudioChannelSet::stereo(), true)
				.withOutput("Out C/D", juce::AudioChannelSet::stereo(), false)
				.withOutput("Out E/F", juce::AudioChannelSet::stereo(), false),
				{"Synthetic audio I/O", "Test", true, true, false, false,
					"Tsio", "urn:gearmulator:test:audio-io", {}, {},
					{0}, {0, 2, 4}})
		{
		}

		synthLib::Device* createDevice() override
		{
			auto* const device = new SyntheticAudioDevice(2, 6, m_nextInputLatency,
				false, 0.125f);
			m_syntheticDevice = device;
			return device;
		}

		pluginLib::Controller* createController() override
		{
			return new SyntheticController(*this);
		}
		juce::AudioProcessorEditor* createEditor() override { return nullptr; }
		bool hasEditor() const override { return false; }
		bool isBusesLayoutSupported(const BusesLayout& _layout) const override
		{
			if(_layout.inputBuses.size() != 1 || _layout.outputBuses.size() != 3)
				return false;
			const auto input = _layout.getMainInputChannelSet();
			if(input != juce::AudioChannelSet::disabled()
				&& input != juce::AudioChannelSet::stereo())
				return false;
			for(int bus = 0; bus < _layout.outputBuses.size(); ++bus)
			{
				const auto channels = _layout.outputBuses[bus];
				if(channels != juce::AudioChannelSet::disabled()
					&& channels != juce::AudioChannelSet::stereo())
					return false;
			}
			return _layout.outputBuses[0] == juce::AudioChannelSet::stereo();
		}

		SyntheticAudioDevice* getSyntheticDevice() const
		{
			return m_syntheticDevice;
		}
		void setNextInputLatency(const uint32_t _latency)
		{
			m_nextInputLatency = _latency;
		}
		void serviceAsyncForTest() { handleAsyncUpdate(); }

	private:
		SyntheticAudioDevice* m_syntheticDevice = nullptr;
		uint32_t m_nextInputLatency = 19;
	};

	class LatencyListener final : public juce::AudioProcessorListener
	{
	public:
		void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
		void audioProcessorChanged(juce::AudioProcessor*,
			const ChangeDetails& _details) override
		{
			if(_details.latencyChanged)
				++latencyChanges;
		}

		std::atomic<uint32_t> latencyChanges{0};
	};

	void verifyModel(const md::MachineModel _model)
	{
		mdJucePlugin::AudioPluginAudioProcessor processor(_model,
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{}, false);

		require(processor.getBusCount(true) == 1,
			"expected one audio input bus");
		require(processor.getBusCount(false) == 3,
			"expected three audio output buses");
		require(processor.getTotalNumInputChannels() == 2,
			"expected two audio input channels");
		require(processor.getTotalNumOutputChannels() == 2,
			"expected only the main output to be enabled by default");

		const std::array<const char*, 1> inputNames{"Input A/B"};
		const std::array<const char*, 3> outputNames{
			"Main A/B", "Out C/D", "Out E/F"};
		for(int bus = 0; bus < static_cast<int>(inputNames.size()); ++bus)
		{
			const auto* const input = processor.getBus(true, bus);
			require(input && input->isEnabled(), "input bus is disabled");
			require(input->getName() == inputNames[bus], "unexpected input bus name");
			require(input->getCurrentLayout() == juce::AudioChannelSet::stereo(),
				"input bus is not stereo");
		}
		for(int bus = 0; bus < static_cast<int>(outputNames.size()); ++bus)
		{
			const auto* const output = processor.getBus(false, bus);
			require(output, "output bus is missing");
			require(output->getName() == outputNames[bus], "unexpected output bus name");
			require(output->isEnabled() == (bus == 0),
				"unexpected default output-bus state");
			require(output->getCurrentLayout() == (bus == 0
				? juce::AudioChannelSet::stereo()
				: juce::AudioChannelSet::disabled()),
				"unexpected default output-bus layout");
		}

		const auto defaultLayout = processor.getBusesLayout();
		require(processor.checkBusesLayoutSupported(defaultLayout),
			"default audio layout was rejected");

		auto layout = defaultLayout;
		layout.outputBuses.set(1, juce::AudioChannelSet::mono());
		require(!processor.checkBusesLayoutSupported(layout),
			"mono individual-output bus was accepted");

		layout = defaultLayout;
		layout.outputBuses.set(2, juce::AudioChannelSet::stereo());
		require(processor.checkBusesLayoutSupported(layout)
			&& processor.setBusesLayout(layout),
			"host could not enable E/F independently");
		require(processor.getTotalNumOutputChannels() == 4
			&& !processor.getBus(false, 1)->isEnabled()
			&& processor.getBus(false, 2)->isEnabled(),
			"E/F-only auxiliary layout was not applied");

		layout = defaultLayout;
		layout.inputBuses.set(0, juce::AudioChannelSet::disabled());
		require(processor.checkBusesLayoutSupported(layout),
			"host could not disable the optional audio input");

		layout = defaultLayout;
		layout.outputBuses.set(1, juce::AudioChannelSet::stereo());
		require(processor.setBusesLayout(layout),
			"host could not enable C/D");
		require(processor.getTotalNumOutputChannels() == 4
			&& processor.getBus(false, 1)->isEnabled()
			&& !processor.getBus(false, 2)->isEnabled(),
			"C/D-only auxiliary layout was not applied");

		layout.outputBuses.set(2, juce::AudioChannelSet::stereo());
		require(processor.setBusesLayout(layout),
			"host could not enable E/F");
		require(processor.getTotalNumOutputChannels() == 6
			&& processor.getBus(false, 2)->isEnabled(),
			"six-output layout was not applied");

		require(processor.setBusesLayout(defaultLayout),
			"host could not return to main-output-only layout");
		require(processor.getTotalNumOutputChannels() == 2,
			"main-output-only layout was not restored");
	}

	void verifyStandaloneLayout(const md::MachineModel _model)
	{
		const auto previousWrapper = juce::PluginHostType::getPluginLoadedAs();
		juce::PluginHostType::jucePlugInClientCurrentWrapperType
			= juce::AudioProcessor::wrapperType_Standalone;
		juce::AudioProcessor::setTypeOfNextNewPlugin(
			juce::AudioProcessor::wrapperType_Standalone);
		auto processor = std::make_unique<mdJucePlugin::AudioPluginAudioProcessor>(
			_model, mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{}, false);
		juce::AudioProcessor::setTypeOfNextNewPlugin(
			juce::AudioProcessor::wrapperType_Undefined);
		juce::PluginHostType::jucePlugInClientCurrentWrapperType = previousWrapper;

		require(processor->wrapperType
			== juce::AudioProcessor::wrapperType_Standalone,
			"processor did not retain the Standalone wrapper type");
		require(processor->getBusCount(false) == 3
			&& processor->getTotalNumOutputChannels() == 2,
			"Standalone construction changed the shared three-bus topology");
		processor->disableNonMainBuses();
		require(processor->getTotalNumOutputChannels() == 2
			&& !processor->getBus(false, 1)->isEnabled()
			&& !processor->getBus(false, 2)->isEnabled(),
			"JUCE Standalone did not retain a stereo main-only layout");

		auto invalid = processor->getBusesLayout();
		invalid.outputBuses.set(0, juce::AudioChannelSet::quadraphonic());
		require(!processor->checkBusesLayoutSupported(invalid),
			"Standalone accepted a non-stereo main bus");
	}

	void verifySparseDeviceCallbacks()
	{
		std::array<float, 32> offsetProbe{};
		require(juce::detail::addAudioCallbackChannelOffset(
			static_cast<float*>(nullptr), 17) == nullptr,
			"standalone callback splitter manufactured an address from null");
		require(juce::detail::addAudioCallbackChannelOffset(
			offsetProbe.data(), 17) == offsetProbe.data() + 17,
			"standalone callback splitter applied the wrong channel offset");

		constexpr int samples = 64;
		SparseCallbackAudioDevice device;
		juce::AudioProcessorPlayer player;
		player.audioDeviceAboutToStart(&device);

		std::array<float, samples> input{};
		std::array<float, samples> output{};
		input.fill(0.5f);
		output.fill(1.0f);
		const float* inputs[] = {nullptr, input.data()};
		float* outputs[] = {output.data(), nullptr};

		// JUCE starts the device callback before attaching the processor. A sparse
		// channel must remain a null sentinel and every real output must be silenced.
		player.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 2, samples, {});
		require(std::all_of(output.begin(), output.end(),
			[](const float value) { return value == 0.0f; }),
			"processor-free sparse callback did not clear the real output");

		SyntheticProcessor processor;
		player.setProcessor(&processor);
		player.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 2, samples, {});
		require(std::all_of(output.begin(), output.end(),
			[](const float value) { return std::isfinite(value); }),
			"sparse input/output callback produced invalid audio");
		player.setProcessor(nullptr);
		player.audioDeviceStopped();

		// Model a transition to a Bluetooth-like device and separate I/O with a
		// single input, stereo output, and a much larger callback. Re-preparation
		// must resize all silence/discard storage before the next callback.
		device.configure(44100.0, 512, 1, 2);
		player.audioDeviceAboutToStart(&device);
		player.setProcessor(&processor);
		std::array<float, 512> bluetoothInput{};
		std::array<float, 512> bluetoothLeft{};
		bluetoothInput.fill(0.25f);
		const float* changedInputs[] = {bluetoothInput.data()};
		float* changedOutputs[] = {bluetoothLeft.data(), nullptr};
		player.audioDeviceIOCallbackWithContext(changedInputs, 1,
			changedOutputs, 2, 512, {});
		require(std::all_of(bluetoothLeft.begin(), bluetoothLeft.end(),
			[](const float value) { return std::isfinite(value); }),
			"device-change callback produced invalid audio");

		// A disappearing device may deliver no backing output array while its
		// stop notification is in flight. This defensive path must remain silent.
		player.setProcessor(nullptr);
		player.audioDeviceIOCallbackWithContext(nullptr, 0, nullptr, 2, 512, {});
		player.audioDeviceStopped();
	}

	void verifyReplacementLatencyNotificationIsAsync()
	{
		SyntheticProcessor processor;
		juce::AudioProcessor& audioProcessor = processor;
		audioProcessor.prepareToPlay(48000.0, 256);
		LatencyListener listener;
		processor.addListener(&listener);
		processor.setNextInputLatency(97);
		const auto previousHostLatency = processor.getLatencySamples();

		std::thread worker([&]
		{
			processor.setDeviceType(pluginLib::DeviceType::Local, true);
		});
		worker.join();
		require(listener.latencyChanges.load() == 0,
			"device replacement notified the host synchronously");
		require(processor.getLatencySamples() == previousHostLatency,
			"device replacement changed host latency on the worker thread");
		processor.serviceAsyncForTest();
		require(listener.latencyChanges.load() > 0,
			"device replacement never notified the host of its new latency");
		require(processor.getLatencySamples() != previousHostLatency,
			"queued latency update did not publish the replacement latency");
		processor.removeListener(&listener);
	}

	void verifyLatencyTracksLayoutRateAndOfflineMode()
	{
		SyntheticProcessor processor;
		juce::AudioProcessor& audioProcessor = processor;
		audioProcessor.prepareToPlay(48000.0, 256);
		LatencyListener listener;
		processor.addListener(&listener);

		const auto inputLatency = processor.getPlugin().getLatencyInputToOutput();
		const auto midiLatency = processor.getPlugin().getLatencyMidiToOutput();
		require(processor.getLatencySamples()
			== static_cast<int>(std::max(inputLatency, midiLatency)),
			"enabled input bus did not contribute to reported latency");

		auto layout = processor.getBusesLayout();
		layout.inputBuses.set(0, juce::AudioChannelSet::disabled());
		require(processor.setBusesLayout(layout),
			"synthetic Processor could not disable its input bus");
		require(processor.getLatencySamples()
			== static_cast<int>(processor.getPlugin().getLatencyMidiToOutput()),
			"disabled input bus retained input-to-output latency");
		require(listener.latencyChanges.load() > 0,
			"input-layout latency change was not published to the host");

		const auto disabledInputLatency48k = processor.getLatencySamples();
		const auto notificationsBeforeOffline = listener.latencyChanges.load();
		audioProcessor.setNonRealtime(true);
		require(processor.getLatencySamples() == disabledInputLatency48k,
			"offline mode alone unexpectedly changed the latency contract");
		require(listener.latencyChanges.load() == notificationsBeforeOffline,
			"offline mode alone emitted a spurious latency notification");
		audioProcessor.prepareToPlay(96000.0, 511);
		require(processor.getLatencySamples()
			== static_cast<int>(processor.getPlugin().getLatencyMidiToOutput()),
			"96 kHz preparation reported stale disabled-input latency");
		require(processor.getLatencySamples() != disabledInputLatency48k,
			"sample-rate change did not recompute host latency");
		require(listener.latencyChanges.load() > notificationsBeforeOffline,
			"sample-rate latency change was not published to the host");

		audioProcessor.setNonRealtime(false);
		layout.inputBuses.set(0, juce::AudioChannelSet::stereo());
		require(processor.setBusesLayout(layout),
			"synthetic Processor could not restore its input bus");
		require(processor.getLatencySamples() == static_cast<int>(std::max(
			processor.getPlugin().getLatencyMidiToOutput(),
			processor.getPlugin().getLatencyInputToOutput())),
			"restored input bus did not restore input-to-output latency");
		processor.removeListener(&listener);
	}

	void verifyInvalidDeviceRecoveryIsDeferred()
	{
		SyntheticProcessor processor;
		juce::AudioProcessor& audioProcessor = processor;
		audioProcessor.prepareToPlay(48000.0, 256);
		auto* const failedDevice = processor.getSyntheticDevice();
		require(failedDevice, "synthetic Processor did not create a device");
		failedDevice->invalidate();
		processor.setNextInputLatency(101);

		juce::MidiBuffer midi;
		juce::AudioBuffer<float> buffer(2, 64);
		buffer.clear();
		audioProcessor.processBlock(buffer, midi);
		require(processor.getSyntheticDevice() == failedDevice,
			"invalid device was replaced synchronously on the audio thread");

		processor.serviceAsyncForTest();
		require(processor.getSyntheticDevice()
			&& processor.getSyntheticDevice()->isValid()
			&& processor.getPlugin().getLatencyInputToOutput() > 101,
			"message-thread recovery did not force-recreate a failed local device");
	}

	void verifyProcessorAudioRouting()
	{
		SyntheticProcessor processor;
		juce::AudioProcessor& audioProcessor = processor;
		audioProcessor.prepareToPlay(48000.0, 256);
		const auto expectedLatency = std::max(
			processor.getPlugin().getLatencyMidiToOutput(),
			processor.getPlugin().getLatencyInputToOutput());
		require(processor.getLatencySamples() == static_cast<int>(expectedLatency),
			"Processor did not report the prepared Plugin latency to the host");

		constexpr int blockSize = 256;
		juce::MidiBuffer midi;
		const std::array<juce::uint8, 4> identityPayload{0x7e, 0x7f, 0x06, 0x01};
		midi.addEvent(juce::MidiMessage::createSysExMessage(
			identityPayload.data(), static_cast<int>(identityPayload.size())), 0);
		const auto midiFallbacks = processor.getRealtimeMidiAllocationFallbackCount();
		juce::AudioBuffer<float> stereoBuffer(2, blockSize);
		for(uint32_t block = 0; block < 4; ++block)
		{
			stereoBuffer.clear();
			for(int sample = 0; sample < blockSize; ++sample)
			{
				stereoBuffer.setSample(0, sample, 0.25f);
				stereoBuffer.setSample(1, sample, 0.5f);
			}
			audioProcessor.processBlock(stereoBuffer, midi);
		}
		require(processor.getRealtimeMidiAllocationFallbackCount()
			== midiFallbacks + 1,
			"host SysEx allocation-capable path was not explicitly accounted");
		require(processor.getSyntheticDevice()
			&& processor.getSyntheticDevice()->receivedEveryOutput(),
			"Processor did not provide every device output buffer");
		for(int channel = 0; channel < stereoBuffer.getNumChannels(); ++channel)
		{
			const auto* const begin = stereoBuffer.getReadPointer(channel);
			const auto audible = std::any_of(begin, begin + blockSize,
				[](const float _sample)
				{
					return std::abs(_sample) > 0.0001f;
				});
			require(audible, "Processor dropped a main output channel");
		}

		auto layout = processor.getBusesLayout();
		layout.outputBuses.set(1, juce::AudioChannelSet::stereo());
		layout.outputBuses.set(2, juce::AudioChannelSet::stereo());
		require(processor.setBusesLayout(layout),
			"synthetic Processor rejected its six-output layout");
		juce::AudioBuffer<float> buffer(6, blockSize);
		for(uint32_t block = 0; block < 4; ++block)
		{
			buffer.clear();
			for(int sample = 0; sample < blockSize; ++sample)
			{
				buffer.setSample(0, sample, 0.25f);
				buffer.setSample(1, sample, 0.5f);
			}
			audioProcessor.processBlock(buffer, midi);
		}
		float previousPeak = 0.0f;
		for(int channel = 0; channel < buffer.getNumChannels(); ++channel)
		{
			const auto* const begin = buffer.getReadPointer(channel);
			const auto audible = std::any_of(begin, begin + blockSize,
				[](const float _sample)
				{
					return std::abs(_sample) > 0.0001f;
				});
			require(audible, "Processor dropped an enabled output channel");
			const auto peak = buffer.getMagnitude(channel, 0, blockSize);
			require(peak > previousPeak,
				"Processor reordered the enabled output channels");
			previousPeak = peak;
		}

		layout.outputBuses.set(1, juce::AudioChannelSet::disabled());
		require(processor.setBusesLayout(layout)
			&& processor.getTotalNumOutputChannels() == 4,
			"synthetic Processor rejected independent E/F routing");
		juce::AudioBuffer<float> efOnlyBuffer(4, blockSize);
		for(uint32_t block = 0; block < 4; ++block)
		{
			efOnlyBuffer.clear();
			for(int sample = 0; sample < blockSize; ++sample)
			{
				efOnlyBuffer.setSample(0, sample, 0.25f);
				efOnlyBuffer.setSample(1, sample, 0.5f);
			}
			audioProcessor.processBlock(efOnlyBuffer, midi);
		}
		const auto abLeft = efOnlyBuffer.getMagnitude(0, 0, blockSize);
		const auto abRight = efOnlyBuffer.getMagnitude(1, 0, blockSize);
		const auto efLeft = efOnlyBuffer.getMagnitude(2, 0, blockSize);
		const auto efRight = efOnlyBuffer.getMagnitude(3, 0, blockSize);
		require(abLeft > 0.0f && abRight > abLeft
			&& efLeft > abRight * 2.0f && efRight > efLeft,
			"independent E/F bus was flattened into the C/D device channels");

		require(processor.getSyntheticDevice()->receivedEveryInput()
			&& processor.getSyntheticDevice()->getInputPeak(0) > 0.0f
			&& processor.getSyntheticDevice()->getInputPeak(1)
				> processor.getSyntheticDevice()->getInputPeak(0),
			"Processor did not preserve both input channels");
	}

	void verifyOutputGainPublication()
	{
		SyntheticProcessor processor;
		require(processor.getOutputGain() == 1.0f,
			"Processor output gain did not retain its default");

		constexpr float lowGain = 0.25f;
		constexpr float highGain = 0.75f;
		processor.setOutputGain(lowGain);
		require(processor.getOutputGain() == lowGain,
			"Processor output gain setter did not publish its value");

		std::array<float, 64> samples;
		samples.fill(1.0f);
		std::array<float*, 1> outputs{samples.data()};
		processor.applyOutputGain(outputs, samples.size());
		require(std::all_of(samples.begin(), samples.end(), [](const float value)
		{
			return value == lowGain;
		}), "Processor did not apply the published output gain");

		std::atomic<bool> start{false};
		std::atomic<bool> finished{false};
		std::atomic<bool> observedInvalidValue{false};
		std::thread writer([&]
		{
			while(!start.load(std::memory_order_acquire))
			{
			}
			for(size_t iteration = 0; iteration < 100000; ++iteration)
				processor.setOutputGain((iteration & 1) == 0 ? highGain : lowGain);
			finished.store(true, std::memory_order_release);
		});
		start.store(true, std::memory_order_release);
		while(!finished.load(std::memory_order_acquire))
		{
			const auto gain = processor.getOutputGain();
			if(gain != lowGain && gain != highGain)
				observedInvalidValue.store(true, std::memory_order_relaxed);
		}
		writer.join();
		require(!observedInvalidValue.load(std::memory_order_relaxed),
			"Concurrent output gain publication produced a torn value");
	}
}

int main()
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		verifyModel(md::MachineModel::Machinedrum);
		verifyModel(md::MachineModel::Monomachine);
		verifyStandaloneLayout(md::MachineModel::Machinedrum);
		verifyStandaloneLayout(md::MachineModel::Monomachine);
		verifySparseDeviceCallbacks();
		verifyProcessorAudioRouting();
		verifyOutputGainPublication();
		verifyLatencyTracksLayoutRateAndOfflineMode();
		verifyInvalidDeviceRecoveryIsDeferred();
		verifyReplacementLatencyNotificationIsAsync();
		verifyIsolatedDataRoot();
		std::cout << "mdAudioIoLayoutTest: PASS\n";
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "mdAudioIoLayoutTest: " << _error.what() << '\n';
		return 1;
	}
}
