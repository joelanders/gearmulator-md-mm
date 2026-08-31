#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "mdLib/mdfrontpanel.h"

namespace mdJucePlugin
{
	// Converts the lossless firmware LED transition stream into a frame-oriented
	// view. A pulse that begins and ends between two UI callbacks remains visible
	// for one slow-renderer frame instead of disappearing into the final snapshot.
	class FrontPanelLedPresentation
	{
	public:
		static constexpr double g_minimumVisibleMilliseconds = 1000.0 / 30.0;

		void reset(const md::FrontPanel& _panel)
		{
			for(uint8_t command = md::FrontPanel::g_firstLedBank;
				command <= md::FrontPanel::g_lastLedBank; ++command)
				m_sourceBanks[bankIndex(command)] = _panel.getLedBankRaw(command);
			m_displayBanks = m_sourceBanks;
			m_holdUntilMilliseconds.fill(0.0);
			m_valid = true;
		}

		void apply(const md::FrontPanelLedTransition& _transition,
			const double _nowMilliseconds)
		{
			if(!m_valid || !validCommand(_transition.command))
				return;

			const auto bank = bankIndex(_transition.command);
			const uint8_t previous = m_sourceBanks[bank];
			const uint8_t rising = static_cast<uint8_t>(previous & ~_transition.value);
			for(uint8_t bit = 0; bit < 8; ++bit)
			{
				if((rising & static_cast<uint8_t>(1u << bit)) == 0)
					continue;
				auto& hold = m_holdUntilMilliseconds[bank * 8 + bit];
				hold = std::max(hold,
					_nowMilliseconds + g_minimumVisibleMilliseconds);
			}
			m_sourceBanks[bank] = _transition.value;
		}

		bool advance(const double _nowMilliseconds)
		{
			if(!m_valid)
				return false;

			auto next = m_sourceBanks;
			for(size_t bank = 0; bank < next.size(); ++bank)
			{
				for(uint8_t bit = 0; bit < 8; ++bit)
				{
					if(_nowMilliseconds < m_holdUntilMilliseconds[bank * 8 + bit])
						next[bank] &= static_cast<uint8_t>(~static_cast<uint8_t>(1u << bit));
				}
			}

			if(next == m_displayBanks)
				return false;
			m_displayBanks = next;
			return true;
		}

		bool valid() const { return m_valid; }

		uint8_t getLedBankRaw(const uint8_t _command) const
		{
			return validCommand(_command)
				? m_displayBanks[bankIndex(_command)] : 0xff;
		}

		bool isLit(const uint8_t _command, const uint8_t _bit) const
		{
			return _bit < 8
				&& (getLedBankRaw(_command) & static_cast<uint8_t>(1u << _bit)) == 0;
		}

	private:
		static constexpr bool validCommand(const uint8_t _command)
		{
			return _command >= md::FrontPanel::g_firstLedBank
				&& _command <= md::FrontPanel::g_lastLedBank;
		}

		static constexpr size_t bankIndex(const uint8_t _command)
		{
			return _command - md::FrontPanel::g_firstLedBank;
		}

		std::array<uint8_t, md::FrontPanel::g_ledBankCount> m_sourceBanks{};
		std::array<uint8_t, md::FrontPanel::g_ledBankCount> m_displayBanks{};
		std::array<double, md::FrontPanel::g_ledBankCount * 8>
			m_holdUntilMilliseconds{};
		bool m_valid = false;
	};
}
