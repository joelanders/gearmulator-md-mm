#include "mdSettingsPanelFeel.h"

#include "mdEditor.h"
#include "mdPixelPerfectPanel.h"

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
		bindGroup(_root, "btWheelSpeed", "panelWheelSpeedPercent");
		bindGroup(_root, "btEncoderSpeed", "panelEncoderSpeedPercent");

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
			startTimerHz(2);
		}
	}

	void SettingsPanelFeel::timerCallback()
	{
		updateRestoreAvailability();
	}

	void SettingsPanelFeel::updateRestoreAvailability()
	{
		if(m_restoreStorage)
			juceRmlUi::helper::setEnabled(m_restoreStorage,
				m_editor.hasStorageRecoveryImage());
	}

	void SettingsPanelFeel::bindGroup(Rml::Element* _root, const char* _idPrefix, const char* _configKey)
	{
		auto& config = m_editor.getProcessor().getConfig();

		std::vector<Rml::Element*> checkboxes(std::size(Editor::g_panelSpeedPercents), nullptr);

		for (size_t i = 0; i < std::size(Editor::g_panelSpeedPercents); ++i)
		{
			auto* row = juceRmlUi::helper::findChild(_root,
				_idPrefix + std::to_string(Editor::g_panelSpeedPercents[i]), false);
			if (row)
				checkboxes[i] = juceRmlUi::helper::findChild(row, "button");
		}

		const auto updateChecked = [checkboxes, &config, _configKey]
		{
			const auto current = config.getIntValue(_configKey, 100);
			for (size_t i = 0; i < checkboxes.size(); ++i)
			{
				if (checkboxes[i])
					juceRmlUi::ElemButton::setChecked(checkboxes[i], Editor::g_panelSpeedPercents[i] == current);
			}
		};

		updateChecked();

		for (const auto percent : Editor::g_panelSpeedPercents)
		{
			auto* row = juceRmlUi::helper::findChild(_root, _idPrefix + std::to_string(percent), false);
			if (!row)
				continue;

			juceRmlUi::EventListener::AddClick(row, [this, updateChecked, &config, _configKey, percent]
			{
				config.setValue(_configKey, percent);
				config.saveIfNeeded();
				updateChecked();
				m_editor.applyPanelSpeeds();
			});
		}
	}
}
