#include <juce_audio_processors/juce_audio_processors.h>

// An actual VST3 wrapper surrounds this firmware-free MIDI oracle. Each PC
// becomes an impulse encoding its channel/program at its exact sample offset.
class ProgramChangeProbe final : public juce::AudioProcessor
{
public:
	ProgramChangeProbe() : AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}
	const juce::String getName() const override { return "MIDI Program Probe"; }
	void prepareToPlay(double, int) override {}
	void releaseResources() override {}
	void processBlock(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
	{
		audio.clear();
		for(int i = 0; i < audio.getNumSamples(); ++i)
			audio.setSample(1, i, static_cast<float>(program));
		for(const auto event : midi)
		{
			const auto message = event.getMessage();
			if(message.isProgramChange())
				audio.setSample(0, event.samplePosition,
					static_cast<float>((message.getChannel() - 1) * 128 + message.getProgramChangeNumber() + 1));
		}
		midi.clear();
	}
	bool acceptsMidi() const override { return true; }
	bool producesMidi() const override { return false; }
	double getTailLengthSeconds() const override { return 0; }
	juce::AudioProcessorEditor* createEditor() override { return nullptr; }
	bool hasEditor() const override { return false; }
	int getNumPrograms() override { return 3; }
	int getCurrentProgram() override { return program; }
	void setCurrentProgram(int value) override { program = value; }
	const juce::String getProgramName(int value) override { return "Preset " + juce::String(value + 1); }
	void changeProgramName(int, const juce::String&) override {}
	void getStateInformation(juce::MemoryBlock& state) override { state.replaceAll(&program, sizeof(program)); }
	void setStateInformation(const void* data, int size) override
	{
		if(size == sizeof(program)) std::memcpy(&program, data, sizeof(program));
	}
private:
	int program = 0;
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ProgramChangeProbe; }
