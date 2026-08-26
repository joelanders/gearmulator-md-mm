#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace md
{
	template<size_t Capacity>
	class FixedByteQueue
	{
	public:
		static_assert(Capacity > 0, "fixed byte queue capacity must be nonzero");

		bool tryPush(const uint8_t _byte)
		{
			return tryPush(&_byte, 1);
		}

		bool tryPush(const uint8_t* const _bytes, const size_t _count)
		{
			if(_bytes == nullptr || _count == 0 || _count > available())
				return false;
			for(size_t i = 0; i < _count; ++i)
				m_bytes[(m_read + m_count + i) % Capacity] = _bytes[i];
			m_count += _count;
			return true;
		}

		bool tryPop(uint8_t& _byte)
		{
			if(empty())
				return false;
			_byte = m_bytes[m_read];
			m_read = (m_read + 1) % Capacity;
			--m_count;
			return true;
		}

		bool tryPeek(uint8_t& _byte) const
		{
			if(empty())
				return false;
			_byte = m_bytes[m_read];
			return true;
		}

		void clear()
		{
			m_read = 0;
			m_count = 0;
		}

		bool empty() const { return m_count == 0; }
		size_t size() const { return m_count; }
		size_t available() const { return Capacity - m_count; }
		static constexpr size_t capacity() { return Capacity; }

	private:
		std::array<uint8_t, Capacity> m_bytes{};
		size_t m_read = 0;
		size_t m_count = 0;
	};
}
