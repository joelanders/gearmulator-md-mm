#pragma once

#include <set>
#include <unordered_set>

namespace rmlPlugin
{
	template<typename Parameter>
	class AutomationGestureState
	{
	public:
		AutomationGestureState() = default;
		AutomationGestureState(const AutomationGestureState&) = delete;
		AutomationGestureState& operator=(const AutomationGestureState&) = delete;

		~AutomationGestureState()
		{
			releasePendingGestures();
		}

		void setMouseIsDown(const void* _owner, const bool _isDown)
		{
			const auto mouseWasDown = getMouseIsDown();

			if(_isDown)
				m_mouseDownOwners.insert(_owner);
			else
				m_mouseDownOwners.erase(_owner);

			if(mouseWasDown && !getMouseIsDown())
				releasePendingGestures();
		}

		bool getMouseIsDown() const noexcept
		{
			return !m_mouseDownOwners.empty();
		}

		void registerPendingGesture(Parameter* _parameter)
		{
			if(!getMouseIsDown() || !_parameter)
				return;
			if(!m_pendingGestures.insert(_parameter).second)
				return;

			_parameter->pushChangeGesture();
		}

		void releasePendingGestures()
		{
			for(auto* parameter : m_pendingGestures)
				parameter->popChangeGesture();

			m_pendingGestures.clear();
		}

	private:
		std::unordered_set<const void*> m_mouseDownOwners;
		std::set<Parameter*> m_pendingGestures;
	};
}
