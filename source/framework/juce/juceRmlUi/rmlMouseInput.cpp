#include "rmlMouseInput.h"

#include "RmlUi/Core/Context.h"

#include <cmath>

namespace
{
	int roundToNearestEven(const double _value)
	{
		const auto lower = std::floor(_value);
		const auto fraction = _value - lower;
		if(fraction < 0.5)
			return static_cast<int>(lower);
		if(fraction > 0.5)
			return static_cast<int>(lower + 1.0);

		const auto lowerInt = static_cast<int>(lower);
		return (lowerInt & 1) == 0 ? lowerInt : lowerInt + 1;
	}
}

namespace juceRmlUi::mouseInput
{
	Rml::Vector2i mapComponentToContextPosition(const int _x, const int _y,
		const int _componentWidth, const int _componentHeight,
		const Rml::Vector2i _contextDimensions)
	{
		if(_componentWidth <= 0 || _componentHeight <= 0)
			return { _x, _y };

		return {
			roundToNearestEven(static_cast<double>(_x)
				* static_cast<double>(_contextDimensions.x) / static_cast<double>(_componentWidth)),
			roundToNearestEven(static_cast<double>(_y)
				* static_cast<double>(_contextDimensions.y) / static_cast<double>(_componentHeight))
		};
	}

	void processButtonDown(Rml::Context& _context, const Rml::Vector2i _position,
		const int _button, const int _modifiers)
	{
		_context.ProcessMouseMove(_position.x, _position.y, _modifiers);
		_context.ProcessMouseButtonDown(_button, _modifiers);
	}

	void processButtonUp(Rml::Context& _context, const Rml::Vector2i _position,
		const int _button, const int _modifiers)
	{
		_context.ProcessMouseMove(_position.x, _position.y, _modifiers);
		_context.ProcessMouseButtonUp(_button, _modifiers);
	}
}
