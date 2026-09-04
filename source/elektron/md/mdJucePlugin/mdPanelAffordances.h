#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "mdLib/mdpanel.h"

// UI policy for the clickable labels and LEDs on the panel skins. It is kept free
// of JUCE and RmlUi so the DOM contract and the navigation maths stay testable
// without building a window or a document.
namespace mdJucePlugin::panelAffordances
{
	// Applied to every clickable label/LED so the skin can style them as one family.
	constexpr const char* g_affordanceClass = "panelAffordance";

	// Indexed DOM id families. The skin checker expands these against the RML.
	constexpr const char* g_drumLedPrefix = "drumLed";
	constexpr const char* g_trackLabelPrefix = "trackLabel";
	constexpr const char* g_trackMutePrefix = "trackMute";

	// A label that issues FUNCTION + control when clicked.
	struct Shortcut
	{
		const char* id;
		md::PanelControl control;
	};

	// Press the control _count times to reach the wanted page/mode.
	struct PulsePlan
	{
		md::PanelControl control;
		int count;
	};

	// Classifies panel presses while Shift is down. The first control is held until
	// Shift is released; subsequent controls remain ordinary momentary actions. The
	// one exception preserves the parameter-lock workflow: a gesture that starts
	// with a trig may collect more trigs, but never a second non-trig modifier.
	class ShiftPanelLatch
	{
	public:
		enum class PressAction
		{
			Momentary,
			Latched,
			Ignored,
		};

		PressAction press(const md::PanelControl _control, const bool _shiftDown)
		{
			const auto index = controlIndex(_control);
			if(m_held[index])
				return PressAction::Ignored;
			if(!_shiftDown)
				return PressAction::Momentary;
			if(m_size != 0 && (!m_triggerSet || !isTrigger(_control)))
				return PressAction::Momentary;

			if(m_size == 0)
				m_triggerSet = isTrigger(_control);
			m_held[index] = true;
			m_order[m_size++] = _control;
			return PressAction::Latched;
		}

		bool contains(const md::PanelControl _control) const
		{
			return m_held[controlIndex(_control)];
		}

		bool empty() const
		{
			return m_size == 0;
		}

		size_t size() const
		{
			return m_size;
		}

		template<typename Release>
		void releaseAll(Release&& _release)
		{
			while(m_size != 0)
			{
				const auto control = m_order[--m_size];
				m_held[controlIndex(control)] = false;
				_release(control);
			}
			m_triggerSet = false;
		}

	private:
		static constexpr size_t g_controlCount =
			static_cast<size_t>(md::PanelControl::ClassicExtended) + 1;

		static constexpr bool isTrigger(const md::PanelControl _control)
		{
			return _control >= md::PanelControl::Trigger1
				&& _control <= md::PanelControl::Trigger16;
		}

		static constexpr size_t controlIndex(const md::PanelControl _control)
		{
			return static_cast<size_t>(_control);
		}

		std::array<bool, g_controlCount> m_held{};
		std::array<md::PanelControl, g_controlCount> m_order{};
		size_t m_size = 0;
		bool m_triggerSet = false;
	};

	// A direct-selection target is replaced, rather than appended, when the user
	// clicks again while the firmware is still processing the previous request.
	// The editor advances one panel pulse at a time and clears the target only
	// after LED readback confirms that it has arrived. One full cycle is the
	// retry limit, so an unresponsive modal screen cannot create an endless loop.
	template<size_t Size>
	class PendingTarget
	{
	public:
		static_assert(Size > 0);

		bool request(const int _target)
		{
			if(_target < 0 || _target >= static_cast<int>(Size))
				return false;

			if(!m_target || *m_target != _target)
				m_attempts = 0;
			m_target = _target;
			return true;
		}

		const std::optional<int>& target() const
		{
			return m_target;
		}

		bool beginAttempt()
		{
			if(!m_target)
				return false;
			if(m_attempts >= Size)
			{
				m_target.reset();
				m_attempts = 0;
				return false;
			}

			++m_attempts;
			return true;
		}

		bool completeIfAt(const int _current)
		{
			if(!m_target || *m_target != _current)
				return false;

			m_target.reset();
			m_attempts = 0;
			return true;
		}

	private:
		std::optional<int> m_target;
		size_t m_attempts = 0;
	};

	constexpr std::array<Shortcut, 11> g_machinedrumShortcuts
	{{
		{ "altMute", md::PanelControl::BankA },
		{ "altAccent", md::PanelControl::BankB },
		{ "altSwing", md::PanelControl::BankC },
		{ "altSlide", md::PanelControl::BankD },
		{ "altLfo", md::PanelControl::SynthesisEffectsRouting },
		{ "altCopy", md::PanelControl::Record },
		{ "altClear", md::PanelControl::Play },
		{ "altPaste", md::PanelControl::Stop },
		{ "altSongSetup", md::PanelControl::Kit },
		{ "altGlobal", md::PanelControl::PatternSong },
		{ "altScaleSetup", md::PanelControl::Scale },
	}};

	constexpr std::array<Shortcut, 12> g_monomachineShortcuts
	{{
		{ "altGlobal", md::PanelControl::Kit },
		{ "altPoly", md::PanelControl::SongEnable },
		{ "altMuteMode", md::PanelControl::BankGroup },
		{ "altArp", md::PanelControl::BankA },
		{ "altTransp", md::PanelControl::BankB },
		{ "altSwing", md::PanelControl::BankC },
		{ "altSlide", md::PanelControl::BankD },
		{ "altCopy", md::PanelControl::Record },
		{ "altClear", md::PanelControl::Play },
		{ "altPaste", md::PanelControl::Stop },
		{ "altMidiSeq", md::PanelControl::TrigSelect },
		{ "altScaleSetup", md::PanelControl::Scale },
	}};

	// Order matters: it is the order the hardware cycles through.
	constexpr std::array<const char*, 3> g_machinedrumDataPages
	{{
		"pageSynthesis", "pageEffects", "pageRouting",
	}};

	constexpr std::array<const char*, 7> g_monomachineDataPages
	{{
		"pageSynthesis", "pageAmplification", "pageFilter", "pageEffects",
		"pageLfo1", "pageLfo2", "pageLfo3",
	}};

	constexpr std::array<const char*, 4> g_monomachineTrigModes
	{{
		"trigModeAmp", "trigModeFilter", "trigModeLfo", "trigModeAll",
	}};

	// LED readback is only trustworthy when exactly one entry is lit.
	template<size_t Size>
	std::optional<int> singleActiveIndex(const std::array<bool, Size>& _active)
	{
		int result = -1;

		for(size_t i = 0; i < Size; ++i)
		{
			if(!_active[i])
				continue;
			if(result >= 0)
				return std::nullopt;
			result = static_cast<int>(i);
		}

		return result >= 0 ? std::optional<int>{result} : std::nullopt;
	}

	// A control that only steps forward, so wrap around to reach an earlier entry.
	inline std::optional<PulsePlan> forwardCyclePlan(const int _current, const int _target,
		const int _size, const md::PanelControl _control)
	{
		if(_size <= 0 || _current < 0 || _current >= _size || _target < 0 || _target >= _size)
			return std::nullopt;

		return PulsePlan{ _control, (_target - _current + _size) % _size };
	}

	// A pair of forward/backward controls, so take whichever direction is shorter.
	inline std::optional<PulsePlan> shortestCyclePlan(const int _current, const int _target,
		const int _size, const md::PanelControl _forward, const md::PanelControl _backward)
	{
		const auto forward = forwardCyclePlan(_current, _target, _size, _forward);
		if(!forward)
			return std::nullopt;
		if(forward->count <= _size / 2)
			return forward;

		return PulsePlan{ _backward, _size - forward->count };
	}

	inline std::optional<PulsePlan> machinedrumDataPagePlan(const int _current, const int _target)
	{
		return forwardCyclePlan(_current, _target, static_cast<int>(g_machinedrumDataPages.size()),
			md::PanelControl::SynthesisEffectsRouting);
	}

	inline std::optional<PulsePlan> monomachineDataPagePlan(const int _current, const int _target)
	{
		return shortestCyclePlan(_current, _target, static_cast<int>(g_monomachineDataPages.size()),
			md::PanelControl::DataPageForward, md::PanelControl::DataPageBackward);
	}

	inline std::optional<PulsePlan> monomachineTrigModePlan(const int _current, const int _target)
	{
		return forwardCyclePlan(_current, _target, static_cast<int>(g_monomachineTrigModes.size()),
			md::PanelControl::TrigSelect);
	}
}
