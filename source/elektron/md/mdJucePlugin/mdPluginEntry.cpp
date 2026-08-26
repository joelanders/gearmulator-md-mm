#include "mdPluginProcessor.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	return new mdJucePlugin::AudioPluginAudioProcessor();
}
