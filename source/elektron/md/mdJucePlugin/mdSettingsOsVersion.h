#pragma once

#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include "juce_events/juce_events.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
	class AudioPluginAudioProcessor;
	class Editor;

	// Device-specific settings: bootable operating-system versions. The
	// dropdown lists the default OS plus every registered variant for this
	// model; picking one reboots the machine after confirmation, carrying
	// user data across. Import registers a .syx upgrade file without
	// rebooting. Each plug-in instance keeps its own choice; remote
	// (stateless-server) devices keep the legacy behavior with the UI off.
	class SettingsOsVersion final : public jucePluginEditorLib::SettingsDeviceSpecific,
		private juce::Timer
	{
	public:
		SettingsOsVersion(Editor& _editor, Rml::Element* _root);
		~SettingsOsVersion() override;

	private:
		void timerCallback() override;
		void refresh(bool _force);
		void onSelect(int _index);

		AudioPluginAudioProcessor* audioProcessor() const;
		static std::string displayLabel(const std::string& _label,
			uint64_t _fingerprint);

		Editor& m_editor;
		juceRmlUi::ElemComboBox* m_combo = nullptr;
		Rml::Element* m_import = nullptr;
		Rml::Element* m_note = nullptr;
		std::vector<uint64_t> m_fingerprints;
		uint64_t m_lastShownFingerprint = 0;
		bool m_lastShownValid = false;
		// Rebuilding the options clamps the widget value, which fires
		// change events for selections the user never made. Ignore them.
		bool m_rebuilding = false;
		// A switch is already confirmed and running; the dropdown shows
		// the previous OS until the reboot commits.
		bool m_switching = false;
		std::shared_ptr<bool> m_aliveToken = std::make_shared<bool>(true);
	};
}
