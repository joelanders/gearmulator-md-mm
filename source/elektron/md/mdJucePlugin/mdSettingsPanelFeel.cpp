#include "mdSettingsPanelFeel.h"

#include "mdEditor.h"
#include "mdLcdInteractionModel.h"
#include "mdPixelPerfectPanel.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginProcessor.h"
#include "jucePluginEditorLib/settingsPlugin.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Element.h"

namespace mdJucePlugin
{
	SettingsPanelFeel::SettingsPanelFeel(Editor& _editor, Rml::Element* _root) : m_editor(_editor)
	{
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btPixelPerfectPanel",
			m_editor.getProcessor().getConfig(), PixelPerfectPanel::configKey, [this](bool)
			{
				m_editor.applyPixelPerfectPanel();
			}, PixelPerfectPanel::defaultEnabled);
		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btLcdRotaryInteraction",
			m_editor.getProcessor().getConfig(), lcdInteraction::configKey, [this](bool)
			{
				m_editor.applyLcdInteraction();
			}, lcdInteraction::defaultEnabled);
		const std::vector<int> speeds(std::begin(Editor::g_panelSpeedPercents), std::end(Editor::g_panelSpeedPercents));
		bindGroup(_root, "btWheelSpeed", "panelWheelSpeedPercent", speeds, 100, [this] { m_editor.applyPanelSpeeds(); });
		bindGroup(_root, "btEncoderSpeed", "panelEncoderSpeedPercent", speeds, 100, [this] { m_editor.applyPanelSpeeds(); });

		jucePluginEditorLib::SettingsPlugin::createToggleButton(_root, "btTooltips",
			m_editor.getProcessor().getConfig(), Editor::g_tooltipsEnabledKey, [this](bool)
			{
				m_editor.applyTooltipSettings();
			}, true);
		bindGroup(_root, "btTooltipDelay", Editor::g_tooltipDelayKey,
			std::vector<int>(std::begin(Editor::g_tooltipDelaysMs), std::end(Editor::g_tooltipDelaysMs)),
			Editor::g_defaultTooltipDelayMs, [this] { m_editor.applyTooltipSettings(); });

		m_ramRecordingComplete = juceRmlUi::helper::findChild(
			_root, "btRamRecordingComplete", false);
		m_ramRecordingOriginal = juceRmlUi::helper::findChild(
			_root, "btRamRecordingOriginal", false);
		if(m_ramRecordingComplete)
			juceRmlUi::EventListener::AddClick(m_ramRecordingComplete, [this]
			{
				static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor())
					.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
				updateRamRecordingMode();
			});
		if(m_ramRecordingOriginal)
			juceRmlUi::EventListener::AddClick(m_ramRecordingOriginal, [this]
			{
				static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor())
					.setRamRecordingMode(md::RamRecordingMode::Original);
				updateRamRecordingMode();
			});
		updateRamRecordingMode();

		if(auto* const loadFactory = juceRmlUi::helper::findChild(
			_root, "btLoadInstalledFactoryStorage", false))
		{
			juceRmlUi::EventListener::AddClick(loadFactory, [this]
			{
				m_editor.loadInstalledFactoryStorage();
			});
		}
		if(auto* const chooseStorage = juceRmlUi::helper::findChild(
			_root, "btChooseStorageImage", false))
		{
			juceRmlUi::EventListener::AddClick(chooseStorage, [this]
			{
				m_editor.chooseStorageImage();
			});
		}
		m_restoreStorage = juceRmlUi::helper::findChild(
			_root, "btRestorePreviousStorage", false);
		if(m_restoreStorage)
		{
			juceRmlUi::EventListener::AddClick(m_restoreStorage, [this]
			{
				m_editor.restorePreviousStorage();
			});
			updateRestoreAvailability();
		}
		if(m_restoreStorage || m_ramRecordingComplete || m_ramRecordingOriginal)
			startTimerHz(2);
	}

	void SettingsPanelFeel::timerCallback()
	{
		updateRestoreAvailability();
		updateRamRecordingMode();
	}

	void SettingsPanelFeel::updateRamRecordingMode()
	{
		if(!m_ramRecordingComplete && !m_ramRecordingOriginal)
			return;
		auto& processor = static_cast<AudioPluginAudioProcessor&>(m_editor.getProcessor());
		const auto mode = processor.getRamRecordingMode();
		const bool available = processor.isRamRecordingModeAvailable();
		if(m_ramRecordingComplete)
		{
			if(auto* const button = juceRmlUi::helper::findChild(
				m_ramRecordingComplete, "button", false))
				juceRmlUi::ElemButton::setChecked(button,
					mode == md::RamRecordingMode::CompleteTail);
			juceRmlUi::helper::setEnabled(m_ramRecordingComplete, available);
		}
		if(m_ramRecordingOriginal)
		{
			if(auto* const button = juceRmlUi::helper::findChild(
				m_ramRecordingOriginal, "button", false))
				juceRmlUi::ElemButton::setChecked(button,
					mode == md::RamRecordingMode::Original);
			juceRmlUi::helper::setEnabled(m_ramRecordingOriginal, available);
		}
	}

	void SettingsPanelFeel::updateRestoreAvailability()
	{
		if(m_restoreStorage)
			juceRmlUi::helper::setEnabled(m_restoreStorage,
				m_editor.hasStorageRecoveryImage());
	}

	void SettingsPanelFeel::bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey,
		std::vector<int> _values, const int _default, std::function<void()> _apply)
	{
		auto& config = m_editor.getProcessor().getConfig();

		std::vector<Rml::Element*> checkboxes(_values.size(), nullptr);
		for (size_t i = 0; i < _values.size(); ++i)
		{
			if (auto* row = juceRmlUi::helper::findChild(_root, _idPrefix + std::to_string(_values[i]), false))
				checkboxes[i] = juceRmlUi::helper::findChild(row, "button");
		}

		const auto updateChecked = [checkboxes, _values, &config, _configKey, _default]
		{
			const auto current = config.getIntValue(_configKey, _default);
			for (size_t i = 0; i < checkboxes.size(); ++i)
			{
				if (checkboxes[i])
					juceRmlUi::ElemButton::setChecked(checkboxes[i], _values[i] == current);
			}
		};

		updateChecked();

		for (const auto value : _values)
		{
			auto* row = juceRmlUi::helper::findChild(_root, _idPrefix + std::to_string(value), false);
			if (!row)
				continue;

			juceRmlUi::EventListener::AddClick(row, [updateChecked, &config, _configKey, value, _apply]
			{
				config.setValue(_configKey, value);
				config.saveIfNeeded();
				updateChecked();
				_apply();
			});
		}
	}
}
