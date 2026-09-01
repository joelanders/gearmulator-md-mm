#pragma once

#include "mdLib/mdtypes.h"

#include <string_view>

namespace mdJucePlugin
{
	constexpr const char* defaultSkinName(const md::MachineModel _model)
	{
		#if JUCE_IOS
		return _model == md::MachineModel::Monomachine ? "mmSfx60" : "mdMobile";
		#else
		return _model == md::MachineModel::Monomachine ? "mmSfx60" : "mdDefault";
		#endif
	}

	constexpr const char* defaultSkinFile(const md::MachineModel _model)
	{
		#if JUCE_IOS
		return _model == md::MachineModel::Monomachine ? "mmSfx60.rml" : "mdMobile.rml";
		#else
		return _model == md::MachineModel::Monomachine ? "mmSfx60.rml" : "mdDefault.rml";
		#endif
	}

	// Preserve support for third-party skins while rejecting a persisted selection
	// that is known to belong to the other Elektron product.
	constexpr bool isSkinCompatible(const md::MachineModel _model,
		const std::string_view _displayName, const std::string_view _filename)
	{
		const auto isMachinedrumSkin = _displayName == "mdDefault" || _filename == "mdDefault.rml"
			|| _displayName == "mdMobile" || _filename == "mdMobile.rml";
		const auto isMonomachineSkin = _displayName == "mmSfx60" || _filename == "mmSfx60.rml";

		return _model == md::MachineModel::Monomachine
			? !isMachinedrumSkin
			: !isMonomachineSkin;
	}
}
