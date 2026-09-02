#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "mdLib/mdfrontpanel.h"

namespace mdJucePlugin
{
	// Turns the lossless firmware transition stream into a frame-oriented view.
	// The first edge seen inside a presentation frame is held long enough to be
	// rendered, whether it is a brief light pulse or a brief dark pulse.
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
			m_heldBanks = m_sourceBanks;
			m_holdUntilMilliseconds.fill(0.0);
			m_valid = true;
		}

		void apply(const md::FrontPanelLedTransition& _transition,
			const double _nowMilliseconds)
		{
			if(!m_valid || !validCommand(_transition.command))
				return;

			const auto bank = bankIndex(_transition.command);
			const uint8_t changed = static_cast<uint8_t>(
				m_sourceBanks[bank] ^ _transition.value);
			for(uint8_t bit = 0; bit < 8; ++bit)
			{
				const uint8_t mask = static_cast<uint8_t>(1u << bit);
				if((changed & mask) == 0)
					continue;

				const auto index = bank * 8 + bit;
				if(_nowMilliseconds < m_holdUntilMilliseconds[index])
					continue;

				// Preserve the first edge in a frame. Later edges still update the
				// source state, but cannot erase this visible pulse before its hold.
				if((_transition.value & mask) != 0)
					m_heldBanks[bank] |= mask;
				else
					m_heldBanks[bank] &= static_cast<uint8_t>(~mask);
				m_holdUntilMilliseconds[index] =
					_nowMilliseconds + g_minimumVisibleMilliseconds;
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
					if(_nowMilliseconds >= m_holdUntilMilliseconds[bank * 8 + bit])
						continue;
					const uint8_t mask = static_cast<uint8_t>(1u << bit);
					if((m_heldBanks[bank] & mask) != 0)
						next[bank] |= mask;
					else
						next[bank] &= static_cast<uint8_t>(~mask);
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
		std::array<uint8_t, md::FrontPanel::g_ledBankCount> m_heldBanks{};
		std::array<double, md::FrontPanel::g_ledBankCount * 8>
			m_holdUntilMilliseconds{};
		bool m_valid = false;
	};
}
