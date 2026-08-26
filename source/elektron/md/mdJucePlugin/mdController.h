#pragma once

#include "jucePluginLib/controller.h"

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor;

	class Controller : public pluginLib::Controller
	{
	public:
		explicit Controller(AudioPluginAudioProcessor& _p);
		~Controller() override;

		// The Machinedrum front panel is driven directly (LCD framebuffer + panel events),
		// not through the parameter/sysex system, so these are minimal no-ops for now.
		void onStateLoaded() override {}

		uint8_t getPartCount() const override { return 1; }

		bool parseSysexMessage(const pluginLib::SysEx&, synthLib::MidiEventSource) override { return false; }

		void sendParameterChange(const pluginLib::Parameter& _parameter, pluginLib::ParamValue _value, pluginLib::Parameter::Origin _origin) override {}

	private:
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Controller)
	};
}
