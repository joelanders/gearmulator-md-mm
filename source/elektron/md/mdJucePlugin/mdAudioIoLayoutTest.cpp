#include "mdPluginProcessor.h"

#include "juce_events/juce_events.h"
#include "jucePluginLib/controller.h"
#include "jucePluginLib/processor.h"
#include "synthLib/device.h"
#include "synthLib/plugin.h"
#include "synthLib/syntheticAudioTestDevice.h"

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

	using SyntheticAudioDevice = synthLib::test::SyntheticAudioDevice;

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
		verifyProcessorAudioRouting();
		verifyLatencyTracksLayoutRateAndOfflineMode();
		verifyInvalidDeviceRecoveryIsDeferred();
		verifyReplacementLatencyNotificationIsAsync();
		std::cout << "mdAudioIoLayoutTest: PASS\n";
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::cerr << "mdAudioIoLayoutTest: " << _error.what() << '\n';
		return 1;
	}
}
