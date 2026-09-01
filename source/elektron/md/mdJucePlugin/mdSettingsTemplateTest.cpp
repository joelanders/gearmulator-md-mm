#include "jucePluginEditorLib/settingsDeviceSpecific.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	bool require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return true;
		std::cerr << "FAIL: " << _message << '\n';
		return false;
	}

	std::string readFile(const std::string& _path)
	{
		std::ifstream stream(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream),
			std::istreambuf_iterator<char>()};
	}

	bool testProduct(const std::string& _productName,
		const std::string& _deviceIdentity,
		const std::string& _filename,
		const std::vector<std::string>& _requiredIds)
	{
		const auto templateNames =
			jucePluginEditorLib::makeDeviceSpecificSettingsTemplateNames(
				"tus_settings_gui", _productName, _deviceIdentity);
		const auto source = readFile(
			std::string(MD_SETTINGS_TEMPLATE_DIR) + '/' + _filename);
		std::string selectedTemplate;
		for(const auto& candidate : templateNames)
		{
			if(!candidate.empty()
				&& source.find("<template name=\"" + candidate + "\">")
					!= std::string::npos)
			{
				selectedTemplate = candidate;
				break;
			}
		}

		bool ok = true;
		ok &= require(!source.empty(), "could not read " + _filename);
		ok &= require(selectedTemplate == "tus_settings_gui_" + _deviceIdentity,
			_productName + " did not resolve its device-specific settings template");
		for(const auto& id : _requiredIds)
		{
			ok &= require(source.find("id=\"" + id + "\"") != std::string::npos,
				_filename + " is missing control " + id);
		}
		return ok;
	}
}

int main()
{
	bool ok = true;
	const auto fallbackNames =
		jucePluginEditorLib::makeDeviceSpecificSettingsTemplateNames(
			"tus_settings_gui", "Example Product", {});
	ok &= require(fallbackNames[0] == "tus_settings_gui_Example Product"
			&& fallbackNames[1].empty(),
		"products without a data identity must retain display-name lookup");

	ok &= testProduct("Gearmulator MD", "Machinedrum",
		"tus_settings_gui_Machinedrum.rml",
		{"btInvertLcdColors", "btSendSysexFile", "sysexDropTarget",
			"sysexTransferStatus", "btImportPlusDrive", "btExportPlusDrive",
			"btRebootMachinedrum", "btResetPlusDrive", "btEncoderSpeed100"});
	ok &= testProduct("Gearmulator MM", "Monomachine",
		"tus_settings_gui_Monomachine.rml",
		{"btInvertLcdColors", "btSendSysexFile", "sysexDropTarget",
			"sysexTransferStatus", "btLoadInstalledFactoryStorage",
			"btEncoderSpeed100"});

	const auto mdSkin = readFile(std::string(MD_SETTINGS_TEMPLATE_DIR)
		+ "/skins/mdDefault/mdDefault.rml");
	ok &= require(!mdSkin.empty(), "could not read the Machinedrum skin");
	for(const auto* const id : {"mdPatternPage0", "mdPatternPage1",
		"mdPatternPage2", "mdPatternPage3", "ledTempo"})
	{
		ok &= require(mdSkin.find("id=\"" + std::string(id) + "\"")
			!= std::string::npos, "Machinedrum skin is missing LED " + std::string(id));
	}

	if(!ok)
		return 1;
	std::cout << "MD/MM device-specific settings templates: PASS\n";
	return 0;
}
