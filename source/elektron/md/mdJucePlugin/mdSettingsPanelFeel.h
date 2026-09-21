#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include "juce_events/juce_events.h"

#include <functional>
#include <vector>

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
		// Radio-style group: element ids are _idPrefix + value, the chosen value is stored under _configKey.
		void bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey,
			std::vector<int> _values, int _default, std::function<void()> _apply);
		void updateRestoreAvailability();
		void updateRamRecordingMode();

		Editor& m_editor;
		Rml::Element* m_restoreStorage = nullptr;
		Rml::Element* m_ramRecordingComplete = nullptr;
		Rml::Element* m_ramRecordingOriginal = nullptr;
	};
}
