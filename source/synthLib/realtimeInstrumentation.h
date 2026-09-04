#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace synthLib
{
	struct RealtimeInstrumentationSnapshot
	{
		bool enabled = false;
		uint64_t outerHostCallbackCount = 0;
		uint64_t outerHostCallbackNanoseconds = 0;
		uint64_t outerHostCallbackMaxNanoseconds = 0;
		// Diagnostic deadline comparison only. Offline/non-realtime host callbacks
		// are included and may legitimately exceed their nominal audio budget.
		uint64_t outerHostCallbackOverrunCount = 0;
		uint64_t outerHostCallbackMaxOverrunNanoseconds = 0;
		uint64_t callbacksWithJitCompilation = 0;
		uint64_t bypassedCallbackCount = 0;
		uint64_t synthProcessCount = 0;
		uint64_t synthProcessNanoseconds = 0;
		uint64_t synthProcessMaxNanoseconds = 0;
		uint64_t synthProcessLockWaitNanoseconds = 0;
		uint64_t synthProcessLockWaitMaxNanoseconds = 0;
		uint64_t resamplerCallCount = 0;
		uint64_t resamplerNanoseconds = 0;
		uint64_t resamplerMaxNanoseconds = 0;
		uint64_t resamplerHostFrames = 0;
		uint64_t resamplingActiveCallbackCount = 0;
		uint64_t deviceProcessNanoseconds = 0;
		uint64_t deviceProcessMaxNanoseconds = 0;
		uint64_t jitCompilationCount = 0;
		uint64_t liveJitCompilationCount = 0;
		uint64_t deferredCandidateJitCompilationCount = 0;
		uint64_t deferredDualMachineCallbackCount = 0;
		uint64_t deferredCandidateAdvanceCount = 0;
		uint64_t deferredCandidateFrames = 0;
		uint64_t deferredCandidateNanoseconds = 0;
		uint64_t deferredCandidateMaxNanoseconds = 0;
		uint64_t callbacksWithAuxOutputBuses = 0;
		uint32_t latestActiveOutputBuses = 0;
		uint32_t maximumActiveOutputBuses = 0;
		uint32_t latestActiveOutputChannels = 0;
		uint32_t maximumActiveOutputChannels = 0;
	};

	// Opt-in counters for diagnosing work performed on the host audio callback.
	// Enable before audio starts with GEARMULATOR_RT_INSTRUMENTATION=1, or use
	// setEnabled() through Plugin::getRealtimeInstrumentation(). No reporting is
	// performed automatically; callers explicitly request a snapshot.
	// Disabled operation is a relaxed atomic load and a predictable branch at each
	// instrumented outer boundary. Clock reads and counter writes happen only while
	// explicitly enabled.
	class RealtimeInstrumentation final
	{
	public:
		class CallbackScope final
		{
		public:
			CallbackScope(RealtimeInstrumentation& _owner, size_t _frames,
				double _sampleRate, bool _bypassed = false) noexcept;
			~CallbackScope();

			CallbackScope(const CallbackScope&) = delete;
			CallbackScope& operator=(const CallbackScope&) = delete;

			bool isActive() const noexcept { return m_owner != nullptr; }
			void setActiveOutputLayout(uint32_t _buses, uint32_t _channels) noexcept;

		private:
			RealtimeInstrumentation* m_owner = nullptr;
			uint64_t m_startNanoseconds = 0;
			uint64_t m_budgetNanoseconds = 0;
			uint32_t m_activeOutputBuses = 0;
			uint32_t m_activeOutputChannels = 0;
			bool m_bypassed = false;
		};

		class DeferredCandidateScope final
		{
		public:
			explicit DeferredCandidateScope(uint32_t _frames) noexcept;
			~DeferredCandidateScope();

			DeferredCandidateScope(const DeferredCandidateScope&) = delete;
			DeferredCandidateScope& operator=(const DeferredCandidateScope&) = delete;

		private:
			RealtimeInstrumentation* m_owner = nullptr;
			uint64_t m_startNanoseconds = 0;
			uint32_t m_frames = 0;
			bool m_previousCandidateRole = false;
		};

		RealtimeInstrumentation() noexcept;

		void setEnabled(bool _enabled) noexcept;
		bool isEnabled() const noexcept
		{
			return m_enabled.load(std::memory_order_relaxed);
		}
		void reset() noexcept;
		// Each field is race-free during concurrent recording/reset, but the result
		// is intentionally a relaxed, non-transactional diagnostic snapshot.
		RealtimeInstrumentationSnapshot snapshot() const noexcept;

		void recordSynthProcess(uint64_t _nanoseconds) noexcept;
		void recordSynthProcessLockWait(uint64_t _nanoseconds) noexcept;
		void recordResampler(uint64_t _nanoseconds, uint64_t _deviceNanoseconds,
			size_t _hostFrames, bool _resamplingActive) noexcept;

		// Called only from the MD DSP JIT's per-block configuration callback. An
		// event is attributed only when a host callback scope is active on this thread.
		static void recordCurrentCallbackJitCompilation() noexcept;

	private:
		friend class CallbackScope;
		friend class DeferredCandidateScope;

		void recordHostCallback(uint64_t _durationNanoseconds,
			uint64_t _budgetNanoseconds, uint64_t _jitCompilations,
			bool _dualMachine, bool _bypassed, uint32_t _outputBuses,
			uint32_t _outputChannels) noexcept;
		void recordDeferredCandidate(uint32_t _frames, uint64_t _nanoseconds) noexcept;

		std::atomic<bool> m_enabled{false};
		std::atomic<uint64_t> m_outerHostCallbackCount{0};
		std::atomic<uint64_t> m_outerHostCallbackNanoseconds{0};
		std::atomic<uint64_t> m_outerHostCallbackMaxNanoseconds{0};
		std::atomic<uint64_t> m_outerHostCallbackOverrunCount{0};
		std::atomic<uint64_t> m_outerHostCallbackMaxOverrunNanoseconds{0};
		std::atomic<uint64_t> m_callbacksWithJitCompilation{0};
		std::atomic<uint64_t> m_bypassedCallbackCount{0};
		std::atomic<uint64_t> m_synthProcessCount{0};
		std::atomic<uint64_t> m_synthProcessNanoseconds{0};
		std::atomic<uint64_t> m_synthProcessMaxNanoseconds{0};
		std::atomic<uint64_t> m_synthProcessLockWaitNanoseconds{0};
		std::atomic<uint64_t> m_synthProcessLockWaitMaxNanoseconds{0};
		std::atomic<uint64_t> m_resamplerCallCount{0};
		std::atomic<uint64_t> m_resamplerNanoseconds{0};
		std::atomic<uint64_t> m_resamplerMaxNanoseconds{0};
		std::atomic<uint64_t> m_resamplerHostFrames{0};
		std::atomic<uint64_t> m_resamplingActiveCallbackCount{0};
		std::atomic<uint64_t> m_deviceProcessNanoseconds{0};
		std::atomic<uint64_t> m_deviceProcessMaxNanoseconds{0};
		std::atomic<uint64_t> m_jitCompilationCount{0};
		std::atomic<uint64_t> m_liveJitCompilationCount{0};
		std::atomic<uint64_t> m_deferredCandidateJitCompilationCount{0};
		std::atomic<uint64_t> m_deferredDualMachineCallbackCount{0};
		std::atomic<uint64_t> m_deferredCandidateAdvanceCount{0};
		std::atomic<uint64_t> m_deferredCandidateFrames{0};
		std::atomic<uint64_t> m_deferredCandidateNanoseconds{0};
		std::atomic<uint64_t> m_deferredCandidateMaxNanoseconds{0};
		std::atomic<uint64_t> m_callbacksWithAuxOutputBuses{0};
		std::atomic<uint32_t> m_latestActiveOutputBuses{0};
		std::atomic<uint32_t> m_maximumActiveOutputBuses{0};
		std::atomic<uint32_t> m_latestActiveOutputChannels{0};
		std::atomic<uint32_t> m_maximumActiveOutputChannels{0};
	};
}
