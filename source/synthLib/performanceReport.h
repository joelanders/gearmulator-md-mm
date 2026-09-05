#pragma once

#include "realtimeInstrumentation.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace synthLib
{
	// Owns the only trace consumer. All lifecycle calls are made off the audio
	// thread; this object must be destroyed before its instrumentation owner.
	class PerformanceReport final
	{
	public:
		enum class Status { Idle, Starting, Recording, Stopped, LimitReached, Error };
		using Context = std::vector<std::pair<std::string, std::string>>;
		struct Limits
		{
			size_t bytes = 8 * 1024 * 1024;
			std::chrono::milliseconds duration{600'000};
			std::chrono::milliseconds flushInterval{2000};
		};
		explicit PerformanceReport(RealtimeInstrumentation& _instrumentation);
		~PerformanceReport();
		void start(std::string _filename, Context _context);
		void start(std::string _filename, Context _context, Limits _limits);
		void stop();
		Status status() const noexcept { return m_status.load(std::memory_order_acquire); }
		static std::string formatContext(const Context& _context);
		static std::string formatSummary(const RealtimeInstrumentationSnapshot& _snapshot, uint64_t _elapsedNanoseconds = 0);
		static std::string formatCallback(const RealtimeSlowCallback& _callback);
	private:
		void run(const std::string& _filename, const Context& _context, Limits _limits) noexcept;
		RealtimeInstrumentation& m_instrumentation;
		std::atomic<Status> m_status{Status::Idle};
		std::atomic<bool> m_stop{false};
		std::mutex m_waitMutex;
		std::condition_variable m_wake;
		std::thread m_thread;
	};
}
