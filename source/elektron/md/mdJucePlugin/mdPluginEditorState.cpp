#include "mdPluginEditorState.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"
#include "mdProductSkinPolicy.h"
#include "mdStandaloneRendererPolicy.h"

#include "mdProductSkins.h"

#include "juce_events/juce_events.h"

namespace mdJucePlugin
{
	PluginEditorState::PluginEditorState(AudioPluginAudioProcessor& _processor)
		: jucePluginEditorLib::PluginEditorState(_processor, _processor.getController(),
			productSkins(_processor.getModel()))
	{
		#if JUCE_MAC
		constexpr bool isMacOS = true;
		#else
		constexpr bool isMacOS = false;
		#endif

		auto& config = _processor.getConfig();
		constexpr auto rendererPreferenceKey = "forceSoftwareRenderer";
		if(shouldPersistStandaloneSoftwareRendererDefault(isMacOS,
			juce::JUCEApplicationBase::isStandaloneApp(),
			_processor.getForceSoftwareRendererForSession().has_value(),
			config.containsKey(rendererPreferenceKey)))
		{
			config.setValue(rendererPreferenceKey, true);
			config.saveIfNeeded();
		}

		const auto configuredSkin = readSkinFromConfig();
		if(configuredSkin.isValid() && isSkinCompatible(_processor.getModel(),
			configuredSkin.displayName, configuredSkin.filename))
		{
			loadSkin(configuredSkin);
			return;
		}

		const auto* const defaultSkin = defaultSkinName(_processor.getModel());

		for(const auto& skin : getIncludedSkins())
		{
			if(skin.displayName == defaultSkin)
			{
				loadSkin(skin);
				return;
			}
		}

		loadDefaultSkin();
	}

	jucePluginEditorLib::Editor* PluginEditorState::createEditor(const jucePluginEditorLib::Skin& _skin)
	{
		return new Editor(m_processor, _skin);
	}
}
