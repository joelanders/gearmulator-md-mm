#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"
#include "juce_events/juce_events.h"

#include <string>

namespace Rml { class Element; }
namespace jucePluginEditorLib { class Processor; }

namespace mdJucePlugin
{
	class SettingsAudioInput final : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsAudioInput(jucePluginEditorLib::Processor& _processor, Rml::Element* _root);
		~SettingsAudioInput() override;

	private:
		void timerCallback() override;

		jucePluginEditorLib::Processor& m_processor;
		Rml::Element* m_status;
		Rml::Element* m_settings;
		std::string m_lastStatus;
	};
}
