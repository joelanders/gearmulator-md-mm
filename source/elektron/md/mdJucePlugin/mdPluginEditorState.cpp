#include "mdPluginEditorState.h"

#include "mdEditor.h"
#include "mdPluginProcessor.h"
#include "mdProductSkinPolicy.h"
#include "mdStandaloneRendererPolicy.h"

#include "mdProductSkins.h"

#include "juce_events/juce_events.h"
#include "juce_gui_basics/juce_gui_basics.h"
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

	auto& bootProcessor = static_cast<AudioPluginAudioProcessor&>(m_processor);
	if(bootProcessor.isBootModeArmed())
	{
		_menu.addEntry("EARLY STARTUP MENU reboot pending…", false, false, {});
	}
	else
	{
		_menu.addEntry("Reboot to EARLY STARTUP MENU…", true, false,
			[this, editor]
			{
				const auto lifetime = editor->getLifetimeToken();
				// Fully async confirm: no nested modal loop inside the RML menu
				// teardown, and the raw button index is logged. Native macOS
				// mapping is zero-based, so the first ("Reboot") button is 0.
				const auto confirmOptions = juce::MessageBoxOptions()
					.withIconType(juce::MessageBoxIconType::QuestionIcon)
					.withTitle("Reboot to EARLY STARTUP MENU")
					.withMessage("The machine reboots into EARLY STARTUP MENU instead of booting normally. Continue?")
					.withButton("Reboot")
					.withButton("Cancel");
				juce::NativeMessageBox::showAsync(confirmOptions, [this, lifetime](int result)
				{
					if(lifetime.expired() || result != 0)
						return;
					auto& processor =
						static_cast<AudioPluginAudioProcessor&>(m_processor);
					if(processor.isBootModeArmed())
						return;
					juce::String actionResult;
					if(processor.rebootToBootMode(actionResult))
					{
						juce::NativeMessageBox::showMessageBoxAsync(
							juce::MessageBoxIconType::InfoIcon,
							"EARLY STARTUP MENU reboot", actionResult, nullptr);
					}
					else if(actionResult.isNotEmpty())
					{
						juce::NativeMessageBox::showMessageBoxAsync(
							juce::MessageBoxIconType::WarningIcon,
							"EARLY STARTUP MENU reboot", actionResult, nullptr);
					}
				});
			});
	}
}
}
