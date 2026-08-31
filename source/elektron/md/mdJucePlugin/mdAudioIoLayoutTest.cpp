#include "mdPluginProcessor.h"

#include "juce_events/juce_events.h"
#include "jucePluginLib/controller.h"
#include "jucePluginLib/processor.h"
#include "synthLib/device.h"
#include "synthLib/plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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

	class SyntheticAudioDevice final : public synthLib::Device
	{
	public:
		SyntheticAudioDevice(const uint32_t _inputs, const uint32_t _outputs,
			const uint32_t _inputLatency, const float _outputLevel = 0.125f)
			: Device({}), m_inputs(_inputs), m_outputs(_outputs),
			m_inputLatency(_inputLatency), m_outputLevel(_outputLevel)
		{
		}

		float getSamplerate() const override { return 44100.0f; }
		bool isValid() const override { return m_valid; }
#if SYNTHLIB_DEMO_MODE == 0
		bool getState(std::vector<uint8_t>&, synthLib::StateType) override
		{
			return false;
		}
		bool setState(const std::vector<uint8_t>&, synthLib::StateType) override
		{
			return false;
		}
#endif
		uint32_t getChannelCountIn() override { return m_inputs; }
		uint32_t getChannelCountOut() override { return m_outputs; }
		uint32_t getInternalLatencyInputToOutput() const override
		{
			return m_inputLatency;
		}
		bool setDspClockPercent(uint32_t) override { return false; }
		uint32_t getDspClockPercent() const override { return 100; }
		uint64_t getDspClockHz() const override { return 100000000; }
		bool receivedEveryOutput() const { return m_receivedEveryOutput; }
		bool receivedEveryInput() const { return m_receivedEveryInput; }
		float getInputPeak(const size_t _channel) const
		{
			return m_inputPeaks[_channel];
		}
		void invalidate() { m_valid = false; }

	protected:
		void readMidiOut(std::vector<synthLib::SMidiEvent>&) override {}
		bool sendMidi(const synthLib::SMidiEvent&,
			std::vector<synthLib::SMidiEvent>&) override
		{
			return true;
		}
		void processAudio(const synthLib::TAudioInputs& _inputs,
			const synthLib::TAudioOutputs& _outputs, const size_t _samples) override
		{
			for(uint32_t channel = 0; channel < m_inputs; ++channel)
			{
				if(!_inputs[channel])
				{
					m_receivedEveryInput = false;
					continue;
				}
				for(size_t sample = 0; sample < _samples; ++sample)
					m_inputPeaks[channel] = std::max(m_inputPeaks[channel],
						std::abs(_inputs[channel][sample]));
			}
			for(uint32_t channel = 0; channel < m_outputs; ++channel)
			{
				if(!_outputs[channel])
				{
					m_receivedEveryOutput = false;
					continue;
				}
				std::fill_n(_outputs[channel], _samples,
					m_outputLevel * static_cast<float>(channel + 1));
			}
		}

	private:
		uint32_t m_inputs;
		uint32_t m_outputs;
		uint32_t m_inputLatency;
		float m_outputLevel;
		bool m_receivedEveryOutput = true;
		bool m_receivedEveryInput = true;
		std::array<float, 4> m_inputPeaks{};
		bool m_valid = true;
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
					"Tsio", "urn:gearmulator:test:audio-io", {}})
		{
		}

		synthLib::Device* createDevice() override
		{
			auto* const device = new SyntheticAudioDevice(2, 6, m_nextInputLatency);
			m_syntheticDevice = device;
			return device;
		}

		pluginLib::Controller* createController() override
		{
			return new SyntheticController(*this);
		}
		juce::AudioProcessorEditor* createEditor() override { return nullptr; }
		bool hasEditor() const override { return false; }

		SyntheticAudioDevice* getSyntheticDevice() const
		{
			return m_syntheticDevice;
		}
		void setNextInputLatency(const uint32_t _latency)
		{
			m_nextInputLatency = _latency;
		}

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
		require(!processor.checkBusesLayoutSupported(layout),
			"enabled output after a disabled bus was accepted");

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
		require(processor->getBusCount(false) == 1
			&& processor->getTotalNumOutputChannels() == 6,
			"Standalone did not expose one six-channel physical-output bus");
		processor->disableNonMainBuses();
		require(processor->getTotalNumOutputChannels() == 6,
			"JUCE Standalone bus filtering hid auxiliary outputs");

		for(const int channelCount : {2, 4, 6})
		{
			auto layout = processor->getBusesLayout();
			layout.outputBuses.set(0,
				juce::AudioChannelSet::canonicalChannelSet(channelCount));
			require(processor->checkBusesLayoutSupported(layout),
				"Standalone rejected a supported physical-output count");
			require(processor->setBusesLayout(layout)
				&& processor->getTotalNumOutputChannels() == channelCount,
				"Standalone could not apply a supported physical-output count");
		}
	}

	void verifyDeviceReplacementAudioTopology()
	{
		auto* const initial = new SyntheticAudioDevice(2, 2, 0);
		auto replacement = std::make_unique<SyntheticAudioDevice>(2, 6, 23);
		{
			synthLib::Plugin plugin(initial,
				[](synthLib::Device* const _device) { return _device; });
			plugin.setHostSamplerate(48000.0f, 44100.0f);
			plugin.setBlockSize(64);
			const auto initialLatency = plugin.getLatencyInputToOutput();

			plugin.setDevice(replacement.get());
			require(plugin.getLatencyInputToOutput() > initialLatency,
				"device replacement did not refresh input latency");

			constexpr size_t blockSize = 256;
			std::array<float, blockSize> leftInput{};
			std::array<float, blockSize> rightInput{};
			const synthLib::TAudioInputs inputs{
				leftInput.data(), rightInput.data(), nullptr, nullptr};
			std::array<std::array<float, blockSize>, 6> outputStorage{};
			synthLib::TAudioOutputs outputs{};
			for(size_t channel = 0; channel < outputStorage.size(); ++channel)
				outputs[channel] = outputStorage[channel].data();
			for(uint32_t block = 0; block < 4; ++block)
				plugin.process(inputs, outputs, blockSize, 120.0f, 0.0f, true);

			require(replacement->receivedEveryOutput(),
				"six-channel replacement received a missing output buffer");
			for(size_t channel = 0; channel < outputStorage.size(); ++channel)
			{
				const auto audible = std::any_of(outputStorage[channel].begin(),
					outputStorage[channel].end(), [](const float _sample)
					{
						return std::abs(_sample) > 0.0001f;
					});
				require(audible,
					"replacement output channel was not routed to the host");
			}
		}
	}

	void verifySameTopologyReplacementFlushesAudio()
	{
		auto* const initial = new SyntheticAudioDevice(2, 6, 0, 0.5f);
		auto replacement = std::make_unique<SyntheticAudioDevice>(2, 6, 0, 0.0f);
		{
			synthLib::Plugin plugin(initial,
				[](synthLib::Device* const _device) { return _device; });
			plugin.setHostSamplerate(48000.0f, 44100.0f);
			plugin.setBlockSize(64);

			constexpr size_t blockSize = 256;
			std::array<float, blockSize> leftInput{};
			std::array<float, blockSize> rightInput{};
			const synthLib::TAudioInputs inputs{
				leftInput.data(), rightInput.data(), nullptr, nullptr};
			std::array<std::array<float, blockSize>, 6> outputStorage{};
			synthLib::TAudioOutputs outputs{};
			for(size_t channel = 0; channel < outputStorage.size(); ++channel)
				outputs[channel] = outputStorage[channel].data();

			plugin.process(inputs, outputs, blockSize, 120.0f, 0.0f, true);
			plugin.setDevice(replacement.get());
			for(auto& output : outputStorage)
				output.fill(0.0f);
			plugin.process(inputs, outputs, blockSize, 120.0f, 0.0f, true);

			for(const auto& output : outputStorage)
			{
				const auto staleAudio = std::any_of(output.begin(), output.end(),
					[](const float _sample)
					{
						return std::abs(_sample) > 0.0001f;
					});
				require(!staleAudio,
					"same-topology replacement emitted stale resampler audio");
			}
		}
	}

	void verifyOutputBusChangesPreserveMainResampler(
		const synthLib::Resampler::Mode _mode)
	{
		auto* const referenceDevice = new SyntheticAudioDevice(2, 6, 0);
		auto* const switchedDevice = new SyntheticAudioDevice(2, 6, 0);
		synthLib::Plugin reference(referenceDevice,
			[](synthLib::Device* const _device) { return _device; });
		synthLib::Plugin switched(switchedDevice,
			[](synthLib::Device* const _device) { return _device; });
		for(auto* const plugin : {&reference, &switched})
		{
			plugin->setHostSamplerate(48000.0f, 44100.0f);
			plugin->setResamplerMode(_mode);
			plugin->setBlockSize(64);
		}

		constexpr size_t blockSize = 127;
		std::array<float, blockSize> leftInput{};
		std::array<float, blockSize> rightInput{};
		const synthLib::TAudioInputs inputs{
			leftInput.data(), rightInput.data(), nullptr, nullptr};
		std::array<std::array<float, blockSize>, 6> referenceStorage{};
		std::array<std::array<float, blockSize>, 6> switchedStorage{};
		synthLib::TAudioOutputs referenceOutputs{};
		synthLib::TAudioOutputs switchedOutputs{};
		for(size_t channel = 0; channel < 6; ++channel)
		{
			referenceOutputs[channel] = referenceStorage[channel].data();
			switchedOutputs[channel] = switchedStorage[channel].data();
		}

		const std::array<uint32_t, 7> switchedCounts{2, 2, 6, 6, 4, 2, 2};
		for(const auto activeChannels : switchedCounts)
		{
			for(auto& output : referenceStorage)
				output.fill(0.0f);
			for(auto& output : switchedStorage)
				output.fill(0.0f);
			reference.process(inputs, referenceOutputs, blockSize,
				120.0f, 0.0f, true, 2);
			switched.process(inputs, switchedOutputs, blockSize,
				120.0f, 0.0f, true, activeChannels);
			for(size_t channel = 0; channel < 2; ++channel)
				require(referenceStorage[channel] == switchedStorage[channel],
					"output-bus change disturbed the main resampler");
		}
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
		processor.removeListener(&listener);
	}

	void verifyInvalidDeviceReplacementRefreshesLatency()
	{
		auto initial = std::make_unique<SyntheticAudioDevice>(2, 6, 0);
		auto replacement = std::make_unique<SyntheticAudioDevice>(2, 6, 31);
		bool replaced = false;
		{
			synthLib::Plugin plugin(initial.get(),
				[&](synthLib::Device* const _device)
				{
					require(_device == initial.get(),
						"invalid-device callback received the wrong device");
					initial.reset();
					replaced = true;
					return replacement.get();
				});
			plugin.setHostSamplerate(48000.0f, 44100.0f);
			plugin.setBlockSize(64);
			const auto initialLatency = plugin.getLatencyInputToOutput();

			constexpr size_t blockSize = 256;
			std::array<float, blockSize> leftInput{};
			std::array<float, blockSize> rightInput{};
			const synthLib::TAudioInputs inputs{
				leftInput.data(), rightInput.data(), nullptr, nullptr};
			std::array<std::array<float, blockSize>, 6> outputStorage{};
			synthLib::TAudioOutputs outputs{};
			for(size_t channel = 0; channel < outputStorage.size(); ++channel)
				outputs[channel] = outputStorage[channel].data();

			initial->invalidate();
			plugin.process(inputs, outputs, blockSize, 120.0f, 0.0f, true);
			require(replaced, "invalid device was not replaced");
			require(plugin.getLatencyInputToOutput() > initialLatency,
				"invalid-device replacement did not refresh Plugin latency");
		}
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
		require(processor.getSyntheticDevice()->receivedEveryInput()
			&& processor.getSyntheticDevice()->getInputPeak(0) > 0.0f
			&& processor.getSyntheticDevice()->getInputPeak(1)
				> processor.getSyntheticDevice()->getInputPeak(0),
			"Processor did not preserve both input channels");
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
		verifyDeviceReplacementAudioTopology();
		verifySameTopologyReplacementFlushesAudio();
		verifyOutputBusChangesPreserveMainResampler(
			synthLib::Resampler::Mode::Legacy);
		verifyOutputBusChangesPreserveMainResampler(
			synthLib::Resampler::Mode::MameHq);
		verifyOutputBusChangesPreserveMainResampler(
			synthLib::Resampler::Mode::MameLofi);
		verifyInvalidDeviceReplacementRefreshesLatency();
		verifyReplacementLatencyNotificationIsAsync();
		verifyProcessorAudioRouting();
		std::cout << "mdAudioIoLayoutTest: PASS\n";
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "mdAudioIoLayoutTest: " << _error.what() << '\n';
		return 1;
	}
}
