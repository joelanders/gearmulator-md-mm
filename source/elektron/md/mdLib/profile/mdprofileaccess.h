#pragma once

#include "mdLib/mdhardware.h"

#include <memory>
#include <vector>

namespace md
{
	// Development-only adapter for the asset-free profiling executable. Keeping
	// this outside Hardware's public API prevents normal product code from
	// bypassing ROM discovery and validation.
	struct ProfileWorkloadAccess
	{
		static std::unique_ptr<Hardware> createHardware(
			const std::vector<uint8_t>& _image, const MachineModel _model)
		{
			return std::unique_ptr<Hardware>(
				new Hardware(true, _image, "generated-profile-input", _model, {}, {}, {}, {}));
		}
	};
}
