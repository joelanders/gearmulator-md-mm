#include "jucePluginLib/controller.h"
#include "jucePluginLib/processor.h"
#include "synthLib/device.h"

#include <array>
#include <vector>

namespace
{
	class ProbeDevice final : public synthLib::Device
	{
	public:
		ProbeDevice() : Device({}) {}
		float getSamplerate() const override { return 48000.0f; }
		bool isValid() const override { return true; }
#if SYNTHLIB_DEMO_MODE == 0
		bool getState(std::vector<uint8_t>&, synthLib::StateType) override { return false; }
		bool setState(const std::vector<uint8_t>&, synthLib::StateType) override { return false; }
#endif
		uint32_t getChannelCountIn() override { return 2; }
		uint32_t getChannelCountOut() override { return 6; }
		bool setDspClockPercent(uint32_t) override { return false; }
		uint32_t getDspClockPercent() const override { return 100; }
		uint64_t getDspClockHz() const override { return 100000000; }

	private:
		void readMidiOut(std::vector<synthLib::SMidiEvent>&) override {}
		bool sendMidi(const synthLib::SMidiEvent&,
			std::vector<synthLib::SMidiEvent>&) override { return true; }
		void processAudio(const synthLib::TAudioInputs& _inputs,
			const synthLib::TAudioOutputs& _outputs, const size_t _samples) override
		{
			for(size_t sample = 0; sample < _samples; ++sample)
			{
				const std::array<float, 2> input{
					_inputs[0][sample], _inputs[1][sample]};
				for(size_t channel = 0; channel < 6; ++channel)
					_outputs[channel][sample] = input[channel & 1]
						+ 0.1f * static_cast<float>(channel);
			}
		}
	};

	class ProbeController final : public pluginLib::Controller
	{
	public:
		explicit ProbeController(pluginLib::Processor& _processor)
			: Controller(_processor) {}
		void sendParameterChange(const pluginLib::Parameter&, pluginLib::ParamValue,
			pluginLib::Parameter::Origin) override {}
		bool parseSysexMessage(const pluginLib::SysEx&,
			synthLib::MidiEventSource) override { return false; }
		void onStateLoaded() override {}
	};

	class ProbeProcessor final : public pluginLib::Processor
	{
	public:
		ProbeProcessor()
			: Processor(BusesProperties()
				.withInput("Input A/B", juce::AudioChannelSet::stereo(), true)
				.withOutput("Main A/B", juce::AudioChannelSet::stereo(), true)
				.withOutput("Out C/D", juce::AudioChannelSet::stereo(), false)
				.withOutput("Out E/F", juce::AudioChannelSet::stereo(), false),
				{"Gearmulator Audio I/O Probe", "Gearmulator", true, true,
					false, false, "Taip", "urn:gearmulator:test:audio-probe",
					{}, {}, {0}, {0, 2, 4}})
		{
		}

		synthLib::Device* createDevice() override { return new ProbeDevice(); }
		pluginLib::Controller* createController() override
		{
			return new ProbeController(*this);
		}
		juce::AudioProcessorEditor* createEditor() override { return nullptr; }
		bool hasEditor() const override { return false; }
		bool isBusesLayoutSupported(const BusesLayout& _layout) const override
		{
			if(_layout.inputBuses.size() != 1 || _layout.outputBuses.size() != 3
				|| _layout.inputBuses[0] != juce::AudioChannelSet::stereo()
				|| _layout.outputBuses[0] != juce::AudioChannelSet::stereo())
				return false;
			for(int bus = 1; bus < 3; ++bus)
				if(_layout.outputBuses[bus] != juce::AudioChannelSet::disabled()
					&& _layout.outputBuses[bus] != juce::AudioChannelSet::stereo())
					return false;
			return true;
		}
	};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new ProbeProcessor();
}
