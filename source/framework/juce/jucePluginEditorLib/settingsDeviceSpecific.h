#pragma once

#include <array>
#include <string>

namespace jucePluginEditorLib
{
	inline std::array<std::string, 2> makeDeviceSpecificSettingsTemplateNames(
		const std::string& _baseTemplateName,
		const std::string& _productName,
		const std::string& _dataFolderName)
	{
		const auto productTemplate = _baseTemplateName + '_' + _productName;
		const auto deviceTemplate = _dataFolderName.empty()
			|| _dataFolderName == _productName
			? std::string{} : _baseTemplateName + '_' + _dataFolderName;
		return {productTemplate, deviceTemplate};
	}

	class SettingsDeviceSpecific
	{
	public:
		SettingsDeviceSpecific() = default;
		virtual ~SettingsDeviceSpecific() = default;
	};
}
