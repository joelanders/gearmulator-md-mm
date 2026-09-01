#include "mdPluginProcessor.h"

#include "jucePluginEditorLib/pluginEditor.h"
#include "jucePluginEditorLib/pluginEditorState.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlHelper.h"

#include "juce_events/juce_events.h"

#include "RmlUi/Core/Element.h"
#include "RmlUi/Core/Context.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		throw std::runtime_error(_message);
	}
}

int main()
{
	try
	{
		juce::ScopedJuceInitialiser_GUI juce;

		#if defined(MD_SETTINGS_TEST_MONOMACHINE)
		constexpr auto model = md::MachineModel::Monomachine;
		const std::vector<std::string> requiredIds{
			"btInvertLcdColors", "btSendSysexFile", "sysexDropTarget",
			"sysexTransferStatus", "btLoadInstalledFactoryStorage",
			"btEncoderSpeed100"};
		#else
		constexpr auto model = md::MachineModel::Machinedrum;
		const std::vector<std::string> requiredIds{
			"btInvertLcdColors", "btSendSysexFile", "sysexDropTarget",
			"sysexTransferStatus", "btImportPlusDrive", "btExportPlusDrive",
			"btRebootMachinedrum", "btResetPlusDrive", "btEncoderSpeed100"};
		#endif

		mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig config;
		config.isolateDeviceStorage = true;
		mdJucePlugin::AudioPluginAudioProcessor processor(model,
			std::move(config), false);
		processor.getConfig().setValue("lcdColorsInverted", false);
		auto& state = processor.getOrCreateEditorState();
		auto* const editor = state.getEditor();
		require(editor != nullptr, "embedded skin did not create an editor");
		editor->showSettings(true);
		require(editor->settingsOpened(), "Settings overlay did not open");
		for(const auto& id : requiredIds)
			require(editor->findChild(id, false) != nullptr,
				"live Settings UI is missing control " + id);
		auto* const invertRoot = editor->findChild("btInvertLcdColors", false);
		auto* const invertButton = dynamic_cast<juceRmlUi::ElemButton*>(
			juceRmlUi::helper::findChild(invertRoot, "button"));
		require(invertButton != nullptr && !invertButton->isChecked(),
			"LCD inversion toggle did not initialize from configuration");
		require(invertRoot->DispatchEvent(Rml::EventId::Click, {}),
			"LCD inversion click was consumed unexpectedly");
		require(processor.getConfig().getBoolValue("lcdColorsInverted", false)
			&& invertButton->isChecked(),
			"LCD inversion click did not update configuration and UI state");
		#if !defined(MD_SETTINGS_TEST_MONOMACHINE)
		for(int page = 0; page < 4; ++page)
		{
			auto* const led = editor->findChild(
				"mdPatternPage" + std::to_string(page), false);
			require(led != nullptr, "live skin is missing MD page LED "
				+ std::to_string(page + 1));
			const auto unlit = led->GetProperty<Rml::Colourb>("image-color");
			led->SetClass("lit", true);
			require(led->GetContext() && led->GetContext()->Update(),
				"live skin did not update after lighting an MD page LED");
			const auto lit = led->GetProperty<Rml::Colourb>("image-color");
			require(lit != unlit, "MD page LED " + std::to_string(page + 1)
				+ " has no visible lit style");
			led->SetClass("lit", false);
		}
		#endif
		processor.destroyEditorState();

		// Recreating the editor must restore the device-specific setting instead of
		// merely keeping transient state on the first checkbox instance.
		auto* const restoredEditor = processor.getOrCreateEditorState().getEditor();
		require(restoredEditor != nullptr, "editor recreation failed");
		restoredEditor->showSettings(true);
		auto* const restoredRoot = restoredEditor->findChild(
			"btInvertLcdColors", false);
		auto* const restoredButton = dynamic_cast<juceRmlUi::ElemButton*>(
			juceRmlUi::helper::findChild(restoredRoot, "button"));
		require(restoredButton != nullptr && restoredButton->isChecked(),
			"LCD inversion setting did not survive editor recreation");
		processor.destroyEditorState();
		std::cout << "Live device-specific Settings UI: PASS\n";
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
