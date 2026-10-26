#pragma once

#include <memory>
#include <string>
#include <vector>

#include "jucePluginEditorLib/settingsPlugin.h"

#include "mdPanelMidiController.h"

#include "juce_events/juce_events.h"

namespace Rml
{
	class Element;
}

namespace juceRmlUi
{
	class ElemComboBox;
}

namespace mdJucePlugin
{
	class Editor;

	// The "Panel MIDI" settings page: the port, the channel, a live view of what
	// the controller sends, and one row per control of this machine with Learn
	// and Clear. Each machine has its own template and its own control list, so
	// the Machinedrum and Monomachine pages show different controls.
	class SettingsPanelMidi : public jucePluginEditorLib::SettingsPlugin,
		private juce::Timer
	{
	public:
		explicit SettingsPanelMidi(Editor& _editor);
		~SettingsPanelMidi() override;

		std::string getCategoryName() const override { return "Panel MIDI"; }
		std::string getTemplateName() const override;

		void createUi(Rml::Element* _root) override;

	private:
		struct Row
		{
			panelMidi::Controller::LearnTarget target;
			Rml::Element* source = nullptr;
			Rml::Element* learn = nullptr;
			juceRmlUi::ElemComboBox* mode = nullptr;	// encoders only
		};

		void timerCallback() override;

		// "Learn all in order" for one section: learns its rows one after another.
		struct Section
		{
			Rml::Element* button = nullptr;
			std::vector<panelMidi::Controller::LearnTarget> targets;
		};

		void createRows(Rml::Element* _root, const char* _rowId,
			const std::vector<panelMidi::Controller::LearnTarget>& _targets);
		void createSection(Rml::Element* _root, const char* _buttonId,
			const std::vector<panelMidi::Controller::LearnTarget>& _targets);
		bool isRunning(const Section& _section, const panelMidi::Controller& _controller) const;
		void refresh();
		panelMidi::Controller* controller() const;

		Editor& m_editor;
		std::weak_ptr<void> m_lifetime;
		std::vector<Row> m_rows;
		std::vector<Section> m_sections;
		juceRmlUi::ElemComboBox* m_channel = nullptr;
		Rml::Element* m_portName = nullptr;
		Rml::Element* m_monitor = nullptr;
		Rml::Element* m_channelWarning = nullptr;

		uint32_t m_revision = ~0u;
		uint32_t m_messageSerial = 0;
		std::string m_shownPortName = "?";
		std::string m_shownChannelWarning = "?";
	};
}
