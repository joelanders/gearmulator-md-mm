#pragma once

#include <array>
#include <optional>
#include <string>

#include "RmlUi/Core/Input.h"

#include "mdLib/mdpanel.h"

namespace mdJucePlugin
{
	struct KeyboardMapping
	{
		Rml::Input::KeyIdentifier key;
		int juceKeyCode;
		md::PanelControl control;
		// When the primary control has no packet for the active model, this is used instead.
		std::optional<md::PanelControl> altControl;
	};

	static constexpr size_t g_keyboardMappingCount = 43;

	const std::array<KeyboardMapping, g_keyboardMappingCount>& defaultKeyboardMappings();

	std::string keyDisplayName(const KeyboardMapping& _mapping);

	Rml::Input::KeyIdentifier rmlKeyForJuceKey(int _juceKey);
	int juceKeyForRmlKey(Rml::Input::KeyIdentifier _key);
}
