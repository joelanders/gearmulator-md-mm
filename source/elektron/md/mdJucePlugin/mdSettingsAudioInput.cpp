#include "mdSettingsAudioInput.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "juce_audio_utils/juce_audio_utils.h"
#include "juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h"

#include "RmlUi/Core/Element.h"

namespace mdJucePlugin
{
	namespace
	{
		juce::StandalonePluginHolder* standaloneHolder(juce::AudioProcessor& _processor)
		{
			auto* const holder = juce::StandalonePluginHolder::getInstance();
			return holder && holder->processor.get() == &_processor ? holder : nullptr;
		}
	}

	SettingsAudioInput::SettingsAudioInput(jucePluginEditorLib::Processor& _processor,
		Rml::Element* _root)
		: m_processor(_processor)
		, m_status(juceRmlUi::helper::findChild(_root, "audioInputStatus"))
		, m_settings(juceRmlUi::helper::findChild(_root, "btAudioInputSettings"))
	{
		juceRmlUi::EventListener::AddClick(m_settings, [this]
		{
			if(auto* const holder = standaloneHolder(m_processor))
				holder->showAudioSettingsDialog();
		});
		timerCallback();
		startTimerHz(2);
	}

	SettingsAudioInput::~SettingsAudioInput()
	{
		stopTimer();
	}

	void SettingsAudioInput::timerCallback()
	{
		auto* const holder = standaloneHolder(m_processor);
		std::string status;
		if(holder)
		{
			if(static_cast<bool>(holder->getMuteInputValue().getValue()))
				status = "Audio input is muted. Open Audio/MIDI Settings and clear Mute audio input to use inputs A/B.";
			else if(auto* const device = holder->deviceManager.getCurrentAudioDevice();
				!device || device->getActiveInputChannels().isZero())
				status = "No audio input channels are active. Select an input device and channels in Audio/MIDI Settings.";
			else
				status = "Audio input is enabled. Route inputs A/B through an input or FX machine and trigger its track to hear it.";
		}
		else
			status = "Your DAW supplies inputs A/B. Route an audio source to this plug-in's stereo input, then select and trigger an input or FX machine.";

		if(status != m_lastStatus)
		{
			m_status->SetInnerRML(Rml::StringUtilities::EncodeRml(status));
			m_settings->SetProperty(Rml::PropertyId::Display,
				holder ? Rml::Style::Display::Block : Rml::Style::Display::None);
			m_lastStatus = std::move(status);
		}
	}
}
