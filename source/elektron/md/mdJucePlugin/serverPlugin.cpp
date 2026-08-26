// ReSharper disable once CppUnusedIncludeDirective
#include "client/plugin.h"

#include "mdLib/mddevice.h"

synthLib::Device* createBridgeDevice(const synthLib::DeviceCreateParams& _params)
{
	return new md::Device(_params);
}
