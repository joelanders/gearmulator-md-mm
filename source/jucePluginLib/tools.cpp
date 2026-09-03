#include "tools.h"

#include "baseLib/filesystem.h"

#include "juce_audio_processors/juce_audio_processors.h"
#include "juce_gui_basics/juce_gui_basics.h"

#include <cstdlib>

namespace pluginLib
{
	bool Tools::isHeadless()
	{
		// returns false on a build machine without display even...
		if(juce::Desktop::getInstance().isHeadless())
			return true;

		const auto host = juce::PluginHostType::getHostPath();

		// So we use this instead. These tools cause crashes if you attempt to
		// open a message box. LV2 even opens the editor, even on a headless
		// build machine, whatever that is good for
		return host.contains("juce_vst3_helper") || host.contains("juce_lv2_helper");
	}

	std::string Tools::getPublicDataFolder(const std::string& _vendorName, const std::string& _productName)
	{
		// Package verification and standalone lifecycle tests must not read or
		// modify a developer's real Documents folder. The release scripts already
		// provide this override; honour it here so those tests exercise a real
		// firmware-backed device instead of silently falling back to DummyDevice.
		const auto* const dataRoot = std::getenv("GEARMULATOR_DATA_ROOT");
		const auto root = dataRoot != nullptr && *dataRoot != '\0'
			? baseLib::filesystem::validatePath(dataRoot)
			: baseLib::filesystem::getSpecialFolderPath(
				baseLib::filesystem::SpecialFolderType::UserDocuments);
		return baseLib::filesystem::validatePath(
			root + _vendorName + '/' + _productName + '/');
	}
}
