#include "mdSettingsPanelFeel.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginProcessor.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlDragTarget.h"
#include "juceRmlUi/rmlEventListener.h"
#include "juceRmlUi/rmlHelper.h"

#include "RmlUi/Core/Element.h"

#include <utility>

namespace mdJucePlugin
{
	class SysexDropTarget final : public juceRmlUi::DragTarget
	{
	public:
		SysexDropTarget(Editor& _editor, Rml::Element* const _element)
			: DragTarget(_element), m_editor(_editor)
		{
			setAllowLocations(false, false);
			setAllowShift(false);
		}

		bool canDropFiles(const Rml::Event&,
			const std::vector<std::string>& _files) override
		{
			return _files.size() == 1 && juce::File(_files.front())
				.getFileExtension().equalsIgnoreCase(".syx");
		}

		void dropFiles(const Rml::Event&, const juceRmlUi::FileDragData*,
			const std::vector<std::string>& _files) override
		{
			if(_files.size() == 1)
				m_editor.sendSysexFile(juce::File(_files.front()));
		}

	private:
		Editor& m_editor;
	};

	SettingsPanelFeel::SettingsPanelFeel(Editor& _editor, Rml::Element* _root) : m_editor(_editor)
	{
		bindGroup(_root, "btWheelSpeed", "panelWheelSpeedPercent");
		bindGroup(_root, "btEncoderSpeed", "panelEncoderSpeedPercent");
		if(auto* const chooseSysex = juceRmlUi::helper::findChild(
			_root, "btSendSysexFile", false))
		{
			juceRmlUi::EventListener::AddClick(chooseSysex, [this]
			{
				m_editor.chooseSysexFile();
			});
		}
		if(auto* const drop = juceRmlUi::helper::findChild(
			_root, "sysexDropTarget", false))
			m_sysexDropTarget = std::make_unique<SysexDropTarget>(m_editor, drop);
		m_sysexStatus = juceRmlUi::helper::findChild(_root, "sysexTransferStatus", false);
		m_plusDriveStatus = juceRmlUi::helper::findChild(_root, "plusDriveStatus", false);
		const auto bind = [_root, this](const char* const _id, auto&& _callback)
		{
			if(auto* const button = juceRmlUi::helper::findChild(_root, _id, false))
				juceRmlUi::EventListener::AddClick(button,
					[this, callback = std::forward<decltype(_callback)>(_callback)]
					{ callback(m_editor); });
		};
		bind("btImportPlusDrive", [](Editor& _editor) { _editor.choosePlusDriveImport(); });
		bind("btExportPlusDrive", [](Editor& _editor) { _editor.choosePlusDriveExport(); });
		bind("btEnablePlusDriveAutoSave",
			[](Editor& _editor) { _editor.choosePlusDriveAutoSave(); });
		bind("btRebootMachinedrum", [](Editor& _editor) { _editor.rebootMachinedrum(); });
		bind("btResetPlusDrive", [](Editor& _editor) { _editor.resetPlusDrive(); });
		m_disablePlusDriveAutoSave = juceRmlUi::helper::findChild(
			_root, "btDisablePlusDriveAutoSave", false);
		if(m_disablePlusDriveAutoSave)
			juceRmlUi::EventListener::AddClick(m_disablePlusDriveAutoSave, [this]
			{ m_editor.disablePlusDriveAutoSave(); });

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
		if(m_sysexStatus)
			startTimerHz(10);
		else if(m_restoreStorage || m_plusDriveStatus)
			startTimerHz(2);
	}

	SettingsPanelFeel::~SettingsPanelFeel() = default;

	void SettingsPanelFeel::timerCallback()
	{
		updateRestoreAvailability();
		if(m_sysexStatus)
			m_sysexStatus->SetInnerRML(m_editor.sysexTransferStatusText());
		if(m_plusDriveStatus)
			m_plusDriveStatus->SetInnerRML(m_editor.plusDriveStatusText());
		if(m_disablePlusDriveAutoSave)
		{
			auto* const processor = dynamic_cast<AudioPluginAudioProcessor*>(
				&m_editor.getProcessor());
			juceRmlUi::helper::setEnabled(m_disablePlusDriveAutoSave,
				processor && processor->plusDriveAutoSaveEnabled());
		}
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
