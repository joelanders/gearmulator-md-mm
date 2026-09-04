#pragma once

#include "jucePluginEditorLib/pluginEditorState.h"

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor;

	class PluginEditorState : public jucePluginEditorLib::PluginEditorState
	{
	public:
		explicit PluginEditorState(AudioPluginAudioProcessor& _processor);

	private:
		jucePluginEditorLib::Editor* createEditor(const jucePluginEditorLib::Skin& _skin) override;
		void initContextMenu(juceRmlUi::Menu& _menu) override;
	};
}
