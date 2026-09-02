#pragma once

#include <vector>

#include "jucePluginEditorLib/skin.h"
#include "mdLib/mdtypes.h"

namespace mdJucePlugin
{
	inline std::vector<jucePluginEditorLib::Skin> productSkins(const md::MachineModel _model)
	{
		if(_model == md::MachineModel::Monomachine)
		{
			return {{"mmSfx60", "mmSfx60.rml", "", {"mmKnob.png",
				"mmKnobAtlas.png", "mmKnobs.rcss", "mmScrew.png",
				"mmBankLed.png", "mmBankLedLit.png", "mmSfx60.rcss",
				"mmSfx60.rml"}}};
		}

		return {{"mdDefault", "mdDefault.rml", "", {"mdDefault.rcss", "mdDefault.rml",
			"mdKnobLineFree.png", "mdKnobs.rcss", "mdScrew.png", "mdSoundWheel.png"}}};
	}
}
