#include "mdPluginProcessor.h"
#include "mdEditor.h"

#include "jucePluginEditorLib/pluginEditor.h"
#include "jucePluginEditorLib/pluginEditorState.h"

#include "juceRmlUi/rmlElemButton.h"
#include "juceRmlUi/rmlHelper.h"

#include "juce_events/juce_events.h"

#include "RmlUi/Core/Element.h"

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
		juce::MessageManagerLock messageLock;
		require(messageLock.lockWasGained(),
			"could not lock the JUCE message thread for live UI testing");

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
		require(processor.getExistingDevice() == nullptr,
			"constructing an ephemeral processor instantiated the device");
		#if !defined(MD_SETTINGS_TEST_MONOMACHINE)
		const auto& binaryData = processor.getProperties().binaryData;
		int cssSize = 0;
		const auto* const cssData = binaryData.getNamedResourceFunc(
			"mdDefault_rcss", cssSize);
		require(cssData != nullptr && cssSize > 0,
			"embedded MD skin has no stylesheet resource");
		const std::string embeddedCss(cssData, static_cast<size_t>(cssSize));
		const auto imageColorInBlock = [&embeddedCss](const size_t _selector)
		{
			if(_selector == std::string::npos)
				return std::string{};
			const auto blockEnd = embeddedCss.find('}', _selector);
			const auto property = embeddedCss.find("image-color:", _selector);
			if(blockEnd == std::string::npos || property >= blockEnd)
				return std::string{};
			const auto value = property + std::string{"image-color:"}.size();
			const auto valueEnd = embeddedCss.find(';', value);
			return valueEnd < blockEnd
				? embeddedCss.substr(value, valueEnd - value) : std::string{};
		};
		const auto litSelector = embeddedCss.find(".mdScaleDot.lit");
		require(litSelector != std::string::npos,
			"embedded MD skin has no lit scale-dot selector");
		const auto unlitSelector = embeddedCss.find(
			".mdModeDot, .mdBankIndicator, .mdScaleDot");
		const auto litColor = imageColorInBlock(litSelector);
		const auto unlitColor = imageColorInBlock(unlitSelector);
		require(!litColor.empty() && !unlitColor.empty() && litColor != unlitColor,
			"embedded MD scale-dot lit and unlit colors are not distinct");
		#endif
		processor.getConfig().setValue("lcdColorsInverted", false);
		auto& state = processor.getOrCreateEditorState();
		require(processor.getExistingDevice() == nullptr,
			"creating editor state instantiated the device");
		auto* const editor = state.getEditor();
		require(editor != nullptr, "embedded skin did not create an editor");
		auto* const deviceEditor = dynamic_cast<mdJucePlugin::Editor*>(editor);
		require(deviceEditor != nullptr, "embedded skin created the wrong editor type");
		editor->showSettings(true);
		require(editor->settingsOpened(), "Settings overlay did not open");
		require(processor.getExistingDevice() == nullptr,
			"opening Settings instantiated the device");
		require(deviceEditor->sysexTransferStatusText() == "MACHINE NOT READY",
			"inactive SysEx status did not report that the machine is not ready");
		require(processor.getExistingDevice() == nullptr,
			"opening Settings or polling SysEx status instantiated the device");
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
			require(led->IsClassSet("mdScaleDot"), "MD page LED "
				+ std::to_string(page + 1) + " has no live scale-dot class");
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
