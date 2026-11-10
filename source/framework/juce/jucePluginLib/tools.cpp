#include "tools.h"

#include <cstdlib>

#include "baseLib/filesystem.h"

namespace pluginLib
{
	std::string Tools::getPublicDataFolder(const std::string& _vendorName, const std::string& _productName)
	{
		// Escape hatch for environments where the user documents folder is not
		// usable. The build-time helpers (juce_vst3_helper & co) instantiate the
		// plugin just to read its metadata, and on the macOS build machine the
		// CI runner is a LaunchAgent, where TCC denies ~/Documents - open() then
		// blocks forever waiting for a consent dialog nobody can answer and the
		// build hangs until the job times out. Redirecting HOME does not help,
		// Xcode rewrites it for script phases, so take the folder explicitly.
		if(const auto* overrideFolder = std::getenv("TUS_DATA_FOLDER"); overrideFolder && *overrideFolder)
			return baseLib::filesystem::validatePath(baseLib::filesystem::validatePath(overrideFolder) + _vendorName + '/' + _productName + '/');

		// Fork: package verification and standalone lifecycle tests must not read or
		// modify a developer's real Documents folder. The release scripts already
		// provide this override; honour it here so those tests exercise a real
		// firmware-backed device instead of silently falling back to DummyDevice.
		if(const auto* dataRoot = std::getenv("GEARMULATOR_DATA_ROOT"); dataRoot && *dataRoot)
			return baseLib::filesystem::validatePath(baseLib::filesystem::validatePath(dataRoot) + _vendorName + '/' + _productName + '/');

		return baseLib::filesystem::validatePath(baseLib::filesystem::getSpecialFolderPath(baseLib::filesystem::SpecialFolderType::UserDocuments) + _vendorName + '/' + _productName + '/');
	}
}
