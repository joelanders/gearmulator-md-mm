#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace md
{
	// Fixed-capacity, nonblocking byte ingress for the single audio-thread producer
	// and single emulation-thread consumer. A write is published only after every
	// byte has been copied, so a rejected command never leaves a partial SysEx in
	// the queue.
	template<size_t Capacity>
	class RealtimeMidiByteQueue
	{
	public:
		static_assert(Capacity > 0, "Realtime MIDI queue capacity must be nonzero");

		bool tryPush(const uint8_t* const _bytes, const size_t _count)
		{
			if(!_bytes || _count == 0 || _count > Capacity)
				return false;

			const auto write = m_write.load(std::memory_order_relaxed);
			const auto read = m_read.load(std::memory_order_acquire);
			if(_count > Capacity - (write - read))
				return false;

			for(size_t i = 0; i < _count; ++i)
				m_bytes[(write + i) % Capacity] = _bytes[i];
			m_write.store(write + _count, std::memory_order_release);
			return true;
		}

		template<size_t Count>
		bool tryPush(const std::array<uint8_t, Count>& _bytes)
		{
			return tryPush(_bytes.data(), _bytes.size());
		}

		bool tryPop(uint8_t& _byte)
		{
			const auto read = m_read.load(std::memory_order_relaxed);
			if(read == m_write.load(std::memory_order_acquire))
				return false;

			_byte = m_bytes[read % Capacity];
			m_read.store(read + 1, std::memory_order_release);
			return true;
		}

		bool tryPeek(uint8_t& _byte) const
		{
			const auto read = m_read.load(std::memory_order_relaxed);
			if(read == m_write.load(std::memory_order_acquire))
				return false;

			_byte = m_bytes[read % Capacity];
			return true;
		}

		bool hasPending() const
		{
			const auto read = m_read.load(std::memory_order_relaxed);
			return read != m_write.load(std::memory_order_acquire);
		}

		size_t size() const
		{
			const auto read = m_read.load(std::memory_order_acquire);
			const auto write = m_write.load(std::memory_order_acquire);
			return write - read;
		}

		// Capture a stable producer boundary without stopping either SPSC endpoint.
		// A consumer can later drain only bytes that existed at this position while
		// leaving subsequently published commands queued.
		size_t writePosition() const
		{
			return m_write.load(std::memory_order_acquire);
		}

		size_t sizeBefore(const size_t _writePosition) const
		{
			const auto read = m_read.load(std::memory_order_acquire);
			return read < _writePosition ? _writePosition - read : 0;
		}

	private:
		std::array<uint8_t, Capacity> m_bytes{};
		std::atomic<size_t> m_write{0};
		std::atomic<size_t> m_read{0};
	};
}
