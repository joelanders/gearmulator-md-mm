#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include "juce_events/juce_events.h"

namespace Rml
{
	class Element;
}

namespace mdJucePlugin
{
	class Editor;

	// Device-specific GUI settings: MM storage selection plus panel encoder feel.
	class SettingsPanelFeel : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsPanelFeel(Editor& _editor, Rml::Element* _root);

	private:
		void timerCallback() override;
		void bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey);
		void updateRestoreAvailability();

		Editor& m_editor;
		Rml::Element* m_restoreStorage = nullptr;
	};
}
