#pragma once

#include <cstdint>

namespace md
{
	// The host receive latch may have been filled by a DSP running ahead of the
	// CPU. Reserve that latch now, but publish its word only at the producer's
	// timestamp. This is one latch, not an additional receive FIFO.
	class TimedHostRx
	{
	public:
		bool pending() const { return m_pending; }

		bool stage(const uint32_t _word, const uint64_t _readyCycle)
		{
			if(m_pending)
				return false;
			m_word = _word;
			m_readyCycle = _readyCycle;
			m_pending = true;
			return true;
		}

		bool take(const uint64_t _now, uint32_t& _word)
		{
			if(!m_pending || _now < m_readyCycle)
				return false;
			_word = m_word;
			m_pending = false;
			return true;
		}

	private:
		uint64_t m_readyCycle = 0;
		uint32_t m_word = 0;
		bool m_pending = false;
	};
}
