#pragma once

#include "juce_gui_basics/juce_gui_basics.h"

#include <functional>
#include <utility>

namespace jucePluginEditorLib::fileChooserFlow
{
	template<typename State>
	bool tryBegin(State& _current, const State _idle, const State _requested)
	{
		if(_current != _idle)
			return false;
		_current = _requested;
		return true;
	}

	// Native chooser and confirmation callbacks run after their initiating stack
	// has unwound. The state pointer is valid only while the lifetime component is
	// alive, so keep the SafePointer check first in the short-circuit expression.
	// A completion consumes its expected flow before calling client code; stale or
	// duplicate callbacks therefore become no-ops.
	template<typename ComponentType, typename State, typename Callback>
	auto makeGuardedCompletion(
		juce::Component::SafePointer<ComponentType> _lifetime,
		State* const _current, const State _expected, const State _idle,
		Callback _callback)
	{
		return [_lifetime, _current, _expected, _idle,
			callback = std::move(_callback)](auto&&... _args) mutable
		{
			if(_lifetime == nullptr || *_current != _expected)
				return false;
			*_current = _idle;
			std::invoke(callback, std::forward<decltype(_args)>(_args)...);
			return true;
		};
	}
}
