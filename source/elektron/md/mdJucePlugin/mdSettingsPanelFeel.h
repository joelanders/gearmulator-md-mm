#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include "juce_events/juce_events.h"

#include <memory>

namespace Rml
{
	class Element;
}

namespace mdJucePlugin
{
	class Editor;
	class SysexDropTarget;

	// Device-specific GUI settings: MM storage selection plus panel encoder feel.
	class SettingsPanelFeel : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsPanelFeel(Editor& _editor, Rml::Element* _root);
		~SettingsPanelFeel() override;

	private:
		void timerCallback() override;
		void bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey);
		void updateRestoreAvailability();

		Editor& m_editor;
		Rml::Element* m_restoreStorage = nullptr;
		Rml::Element* m_sysexStatus = nullptr;
		Rml::Element* m_plusDriveStatus = nullptr;
		Rml::Element* m_disablePlusDriveAutoSave = nullptr;
		std::unique_ptr<SysexDropTarget> m_sysexDropTarget;
	};
}
