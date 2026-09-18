#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"
#include "jucePluginEditorLib/settingsPlugin.h"

#include "juceRmlUi/rmlEventListener.h"

#include "mdKeyBindings.h"

#include <vector>

namespace Rml
{
	class Element;
	class Event;
}

namespace jucePluginEditorLib
{
	class Processor;
}

namespace mdJucePlugin
{
	class Editor;

	class SettingsPluginKeyBindings : public jucePluginEditorLib::SettingsPlugin
	{
	public:
		explicit SettingsPluginKeyBindings(jucePluginEditorLib::Processor& _processor)
			: SettingsPlugin(_processor) {}

		std::string getCategoryName() const override { return "Key Bindings"; }
		std::string getTemplateName() const override { return "tus_settings_keybinds"; }
		void createUi(Rml::Element*) override {}
	};

	class SettingsKeyBindings : public jucePluginEditorLib::SettingsDeviceSpecific
	{
	public:
		SettingsKeyBindings(Editor& _editor, Rml::Element* _root);

		bool onKeyDown(Rml::Event& _event);

	private:
		void buildRows();
		void updateRowDisplay(size_t _index);
		void enterCaptureMode(size_t _index);
		void exitCaptureMode();
		void applyCapture(Rml::Input::KeyIdentifier _rmlKey, int _juceKey);
		void resetToDefaults();

		Editor& m_editor;
		Rml::Element* m_listContainer = nullptr;
		Rml::Element* m_rowTemplate = nullptr;
		Rml::Element* m_resetStatusElem = nullptr;
		std::vector<Rml::Element*> m_rows;
		int m_capturingIndex = -1;
		juceRmlUi::ScopedListener m_keydownListener;
	};
}
