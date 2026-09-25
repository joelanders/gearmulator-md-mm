#include "mdPluginEditorState.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"
#include "mdProductSkinPolicy.h"
#include "mdStandaloneRendererPolicy.h"

#include "mdProductSkins.h"

#include "juce_events/juce_events.h"
#include "jucePluginEditorLib/rendererPreferenceKeys.h"
#include "juceRmlUi/rmlMenu.h"

namespace mdJucePlugin
{
	PluginEditorState::PluginEditorState(AudioPluginAudioProcessor& _processor)
		: jucePluginEditorLib::PluginEditorState(_processor, _processor.getController(),
			productSkins(_processor.getModel()))
	{
		#if JUCE_MAC
		constexpr bool isMacOS = true;
		#else
		constexpr bool isMacOS = false;
		#endif

		auto& config = _processor.getConfig();
		using namespace jucePluginEditorLib;
		if(shouldRemoveLegacyStandaloneSoftwareRenderer(isMacOS,
			juce::JUCEApplicationBase::isStandaloneApp(),
			_processor.getForceSoftwareRendererForSession().has_value(),
			config.getBoolValue(forceSoftwareRendererUserSelectedKey, false),
			config.containsKey(forceSoftwareRendererKey),
			config.getBoolValue(forceSoftwareRendererKey, false)))
		{
			config.removeValue(forceSoftwareRendererKey);
			config.saveIfNeeded();
		}

		const auto configuredSkin = readSkinFromConfig();
		if(configuredSkin.isValid() && isSkinCompatible(_processor.getModel(),
			configuredSkin.displayName, configuredSkin.filename))
		{
			loadSkin(configuredSkin);
			return;
		}

		const auto* const defaultSkin = defaultSkinName(_processor.getModel());

		for(const auto& skin : getIncludedSkins())
		{
			if(skin.displayName == defaultSkin)
			{
				loadSkin(skin);
				return;
			}
		}

		loadDefaultSkin();
	}

	jucePluginEditorLib::Editor* PluginEditorState::createEditor(const jucePluginEditorLib::Skin& _skin)
	{
		return new Editor(m_processor, _skin);
	}

	void PluginEditorState::initContextMenu(juceRmlUi::Menu& _menu)
	{
		jucePluginEditorLib::PluginEditorState::initContextMenu(_menu);
		auto& processor = static_cast<AudioPluginAudioProcessor&>(m_processor);
		if(processor.getModel() == md::MachineModel::Machinedrum)
		{
			const bool available = processor.isRamRecordingModeAvailable();
			const auto mode = processor.getRamRecordingMode();
			juceRmlUi::Menu ramRecording;
			ramRecording.addEntry("Complete tails (recommended)", available,
				mode == md::RamRecordingMode::CompleteTail, [&processor]
				{
					processor.setRamRecordingMode(md::RamRecordingMode::CompleteTail);
				});
			ramRecording.addEntry("Original finalization", available,
				mode == md::RamRecordingMode::Original, [&processor]
				{
					processor.setRamRecordingMode(md::RamRecordingMode::Original);
				});
			_menu.addSubMenu("RAM recording", std::move(ramRecording));
		}
		juceRmlUi::Menu diagnostics;
		diagnostics.addEntry(processor.performanceDiagnosticsActive()
			? "Stop performance capture" : "Start performance capture", [this]
			{
				auto& processor = static_cast<AudioPluginAudioProcessor&>(m_processor);
				processor.setPerformanceDiagnosticsEnabled(!processor.performanceDiagnosticsActive());
			});
		diagnostics.addEntry("Open logs folder", [folder = processor.performanceDiagnosticsFolder()]
			{
				// Open Finder/Explorer after the Rml menu has closed.
				juce::MessageManager::callAsync([folder]
					{
						if(folder.createDirectory().wasOk()) folder.revealToUser();
					});
			});
		diagnostics.addSeparator();
		diagnostics.addEntry(processor.performanceDiagnosticsStatus(), false, false, {});
		_menu.addSubMenu("Performance diagnostics", std::move(diagnostics));

		auto* const editor = dynamic_cast<Editor*>(getEditor());
		if(!editor)
			return;

#if GEARMULATOR_MDMM_RAM_DIAGNOSTICS
		_menu.addEntry("Show RAM diff panel", true, editor->ramDiffEnabled(), [editor]
			{
				editor->setRamDiffEnabled(!editor->ramDiffEnabled());
			});
#endif

		const bool active = editor->isUserSysexTransferActive();
		if(editor->canResumeUserSysexTransfer())
			_menu.addEntry("Resume SysEx Transfer - machine is ready", true, false,
				[editor] { editor->resumeUserSysexTransfer(); });
		const bool cancellable = editor->canCancelUserSysexTransfer();
		_menu.addEntry(editor->getUserSysexMenuText(),
			!active || cancellable, false, [this, editor, cancellable]
			{
				if(cancellable)
				{
					editor->cancelUserSysexTransfer();
					return;
				}
				// Menu actions run before the Rml menu closes. Defer the native picker
				// until that teardown has completed.
				const auto lifetime = editor->getLifetimeToken();
				juce::MessageManager::callAsync([lifetime, editor]
				{
					if(!lifetime.expired())
						editor->chooseUserSysexFile();
				});
			});
	}
}

