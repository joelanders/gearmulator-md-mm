#include "mdController.h"

#include "mdPluginProcessor.h"

namespace mdJucePlugin
{
	Controller::Controller(AudioPluginAudioProcessor& _p) : pluginLib::Controller(_p)
	{
		registerParams(_p);
	}

	Controller::~Controller() = default;
}
