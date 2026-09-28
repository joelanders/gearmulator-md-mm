#include "mdSettingsOsVersion.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"

#include "mdLib/mdosvariant.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "jucePluginLib/types.h"
#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/rmlElemComboBox.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"
#include "juceUiLib/messageBox.h"

#include "RmlUi/Core/Element.h"

#include <algorithm>
#include <cstdio>

namespace mdJucePlugin
{
	namespace
	{
		constexpr char kOsVariantCombo[] = "osVariantList";
		constexpr char kOsVariantImport[] = "btImportOsVariant";
		constexpr char kOsVariantNote[] = "osVariantNote";
	}

	SettingsOsVersion::SettingsOsVersion(Editor& _editor, Rml::Element* _root)
		: m_editor(_editor)
		, m_combo(juceRmlUi::helper::findChildT<juceRmlUi::ElemComboBox>(
			_root, kOsVariantCombo))
		, m_import(juceRmlUi::helper::findChild(_root, kOsVariantImport, false))
		, m_note(juceRmlUi::helper::findChild(_root, kOsVariantNote, false))
	{
		if(m_combo)
		{
			m_combo->onValueChanged.addListener([this](const float _value)
			{
				onSelect(static_cast<int>(_value));
			});
		}
		if(m_import)
		{
			juceRmlUi::EventListener::AddClick(m_import, [this]
			{
				const auto token = m_aliveToken;
				m_editor.importOsVariantFile([this, token]
				{
					if(*token)
						refresh(true);
				});
			});
		}
		refresh(true);
		startTimerHz(2);
	}

	SettingsOsVersion::~SettingsOsVersion()
	{
		*m_aliveToken = false;
		stopTimer();
	}

	AudioPluginAudioProcessor* SettingsOsVersion::audioProcessor() const
	{
		return dynamic_cast<AudioPluginAudioProcessor*>(
			&m_editor.getProcessor());
	}

	std::string SettingsOsVersion::displayLabel(const std::string& _label,
		const uint64_t _fingerprint)
	{
		if(!_label.empty())
			return _label;
		char fingerprint[20];
		std::snprintf(fingerprint, sizeof(fingerprint), "%016llx",
			static_cast<unsigned long long>(_fingerprint));
		return "Custom OS " + std::string(fingerprint, fingerprint + 8);
	}

	void SettingsOsVersion::timerCallback()
	{
		refresh(false);
	}

	void SettingsOsVersion::refresh(const bool _force)
	{
		if(!m_combo)
			return;
		auto* const processor = audioProcessor();
		const auto current = processor ? processor->getOsVariantFingerprint() : 0;
		if(!_force && m_lastShownValid && current == m_lastShownFingerprint)
			return;

		m_rebuilding = true;
		m_fingerprints.clear();
		m_combo->clearOptions();
		m_fingerprints.push_back(0);
		m_combo->addOption("Default OS");
		const auto local = processor
			&& processor->getDeviceType() == pluginLib::DeviceType::Local;
		if(local)
		{
			std::vector<md::OsVariantInfo> variants;
			if(md::listOsVariants(variants, processor->getDeviceHomePath()))
			{
				std::sort(variants.begin(), variants.end(),
					[](const md::OsVariantInfo& _a, const md::OsVariantInfo& _b)
					{
						if(_a.model != _b.model)
							return _a.model < _b.model;
						if(_a.label != _b.label)
							return _a.label < _b.label;
						return _a.fingerprint < _b.fingerprint;
					});
				for(const auto& variant : variants)
				{
					if(variant.model != processor->getModel())
						continue;
					m_fingerprints.push_back(variant.fingerprint);
					m_combo->addOption(displayLabel(variant.label,
						variant.fingerprint));
				}
			}
		}

		auto index = std::find(m_fingerprints.begin(), m_fingerprints.end(),
			current);
		if(index == m_fingerprints.end() && current != 0)
		{
			// The remembered OS is no longer registered (its file was
			// removed): show the truth rather than silently pointing at the
			// default. Switching away works; switching back to it fails
			// visibly in the processor.
			m_fingerprints.push_back(current);
			m_combo->addOption("Missing OS " + displayLabel({}, current));
			index = m_fingerprints.end() - 1;
		}
		m_combo->setSelectedIndex(static_cast<size_t>(
			index == m_fingerprints.end() ? 0 : index - m_fingerprints.begin()),
			false);
		m_rebuilding = false;
		m_lastShownFingerprint = current;
		m_lastShownValid = true;

		const bool enabled = local && !m_switching;
		juceRmlUi::helper::setEnabled(m_combo, enabled);
		if(m_import)
			juceRmlUi::helper::setEnabled(m_import, enabled);
		if(m_note)
		{
			m_note->SetInnerRML(local ? Rml::String{}
				: "OS versions need a local device. This instance runs on a remote server, which always boots the default OS.");
		}
	}

	void SettingsOsVersion::onSelect(const int _index)
	{
		// Selections the user never made arrive while the options are
		// rebuilt (re-clamping the widget value fires change events) or
		// while a confirmed switch is running: never act on them.
		if(m_rebuilding || m_switching)
			return;
		auto* const processor = audioProcessor();
		if(!processor || _index < 0
			|| static_cast<size_t>(_index) >= m_fingerprints.size())
			return;
		const auto fingerprint = m_fingerprints[static_cast<size_t>(_index)];
		const auto current = processor->getOsVariantFingerprint();
		// Collapse the visual selection back to the running OS first: the
		// dropdown never displays an unconfirmed choice, so any stale or
		// duplicate event for this gesture becomes a no-op below.
		const auto shown = std::find(m_fingerprints.begin(),
			m_fingerprints.end(), current);
		if(shown != m_fingerprints.end())
		{
			m_rebuilding = true;
			m_combo->setSelectedIndex(
				static_cast<size_t>(shown - m_fingerprints.begin()), false);
			m_rebuilding = false;
		}
		if(fingerprint == current)
			return;
		std::string name("the default OS");
		if(fingerprint != 0)
		{
			name = "'" + displayLabel({}, fingerprint) + "'";
			std::vector<md::OsVariantInfo> variants;
			if(md::listOsVariants(variants, processor->getDeviceHomePath()))
			{
				for(const auto& variant : variants)
				{
					if(variant.fingerprint == fingerprint
						&& variant.model == processor->getModel()
						&& !variant.label.empty())
						name = "'" + variant.label + "'";
				}
			}
		}
		const auto message = "Reboot into " + name + " now?\n\n"
			"The machine reboots immediately; kits, patterns and songs are carried across.";
		m_switching = true;
		refresh(true);
		genericUI::MessageBox::showYesNo(genericUI::MessageBox::Icon::Warning,
			"Reboot into another OS?", message, [this, fingerprint](const genericUI::MessageBox::Result _answer)
			{
				m_switching = false;
				if(_answer != genericUI::MessageBox::Result::Yes)
				{
					refresh(true);
					return;
				}
				auto* const processor = audioProcessor();
				if(!processor || fingerprint == processor->getOsVariantFingerprint())
				{
					refresh(true);
					return;
				}
				juce::String result;
				// Runs the reboot synchronously like the storage-image flow:
				// the message thread is blocked, so this page cannot close
				// underneath the completion.
				const bool switched = processor->switchOsVariant(fingerprint,
					result);
				// Success needs no popup: the dropdown now shows the running
				// OS. Only a failure interrupts with one.
				if(!switched)
				{
					genericUI::MessageBox::showOk(
						genericUI::MessageBox::Icon::Warning, "OS not switched",
						result.toStdString(), m_editor.getRmlComponent());
				}
				refresh(true);
			});
	}
}
