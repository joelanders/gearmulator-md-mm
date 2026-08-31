#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mdJucePlugin
{
	// Bounded multi-producer/single-or-multi-consumer queue based on per-cell
	// sequence numbers. Producers and consumers never wait, allocate, or take a
	// lock; callers decide whether a full queue should be retried or reported.
	template<typename T, size_t Capacity>
	class RealtimeQueue
	{
		static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0,
			"RealtimeQueue capacity must be a power of two");
		static_assert(std::is_trivially_copyable_v<T>,
			"RealtimeQueue entries must be trivially copyable");
		static_assert(std::atomic<size_t>::is_always_lock_free,
			"RealtimeQueue requires lock-free size_t atomics");

		struct Cell
		{
			std::atomic<size_t> sequence{0};
			T value{};
		};

	public:
		RealtimeQueue()
		{
			for(size_t index = 0; index < Capacity; ++index)
				m_cells[index].sequence.store(index, std::memory_order_relaxed);
		}

		bool tryPush(const T& _value)
		{
			auto position = m_enqueuePosition.load(std::memory_order_relaxed);
			for(;;)
			{
				auto& cell = m_cells[position & (Capacity - 1)];
				const auto sequence = cell.sequence.load(std::memory_order_acquire);
				const auto difference = static_cast<intptr_t>(sequence)
					- static_cast<intptr_t>(position);
				if(difference == 0)
				{
					if(m_enqueuePosition.compare_exchange_weak(position, position + 1,
						std::memory_order_relaxed, std::memory_order_relaxed))
					{
						cell.value = _value;
						cell.sequence.store(position + 1, std::memory_order_release);
						return true;
					}
				}
				else if(difference < 0)
				{
					return false;
				}
				else
				{
					position = m_enqueuePosition.load(std::memory_order_relaxed);
				}
			}
		}

		bool tryPop(T& _value)
		{
			auto position = m_dequeuePosition.load(std::memory_order_relaxed);
			for(;;)
			{
				auto& cell = m_cells[position & (Capacity - 1)];
				const auto sequence = cell.sequence.load(std::memory_order_acquire);
				const auto difference = static_cast<intptr_t>(sequence)
					- static_cast<intptr_t>(position + 1);
				if(difference == 0)
				{
					if(m_dequeuePosition.compare_exchange_weak(position, position + 1,
						std::memory_order_relaxed, std::memory_order_relaxed))
					{
						_value = cell.value;
						cell.sequence.store(position + Capacity,
							std::memory_order_release);
						return true;
					}
				}
				else if(difference < 0)
				{
					return false;
				}
				else
				{
					position = m_dequeuePosition.load(std::memory_order_relaxed);
				}
			}
		}

	private:
		// Do not give the owning Controller an over-aligned type: the plugin still
		// supports deployment targets before macOS 10.13, where aligned new/delete
		// are unavailable. Correctness only requires the atomics themselves to have
		// their natural alignment; cache-line separation would be an optimization.
		std::array<Cell, Capacity> m_cells{};
		std::atomic<size_t> m_enqueuePosition{0};
		std::atomic<size_t> m_dequeuePosition{0};
	};
}
