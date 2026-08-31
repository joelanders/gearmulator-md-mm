#pragma once

#include "RmlUi/Core/Types.h"

namespace Rml
{
	class Context;
}

namespace juceRmlUi::mouseInput
{
	Rml::Vector2i mapComponentToContextPosition(int _x, int _y,
		int _componentWidth, int _componentHeight,
		Rml::Vector2i _contextDimensions);

	void processButtonDown(Rml::Context& _context, Rml::Vector2i _position,
		int _button, int _modifiers);
	void processButtonUp(Rml::Context& _context, Rml::Vector2i _position,
		int _button, int _modifiers);
}
