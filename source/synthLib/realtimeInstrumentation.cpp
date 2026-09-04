#include "realtimeInstrumentation.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace
{
	using Clock = std::chrono::steady_clock;

	uint64_t nowNanoseconds() noexcept
	{
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now().time_since_epoch()).count());
	}

	template<typename T>
	void updateMaximum(std::atomic<T>& _target, const T _value) noexcept
	{
		auto previous = _target.load(std::memory_order_relaxed);
		while(previous < _value && !_target.compare_exchange_weak(previous, _value,
			std::memory_order_relaxed, std::memory_order_relaxed))
		{
		}
	}

	struct CallbackContext
	{
		synthLib::RealtimeInstrumentation* owner = nullptr;
		uint64_t jitCompilations = 0;
		bool dualMachine = false;
		bool candidateRole = false;
	};

	thread_local CallbackContext g_callbackContext;

	bool enabledFromEnvironment() noexcept
	{
		const auto* const value = std::getenv("GEARMULATOR_RT_INSTRUMENTATION");
		return value != nullptr && (std::string_view(value) == "1"
			|| std::string_view(value) == "true"
			|| std::string_view(value) == "TRUE");
	}
}

namespace synthLib
{
	RealtimeInstrumentation::RealtimeInstrumentation() noexcept
		: m_enabled(enabledFromEnvironment())
	{
	}

	void RealtimeInstrumentation::setEnabled(const bool _enabled) noexcept
	{
		m_enabled.store(_enabled, std::memory_order_relaxed);
	}

	void RealtimeInstrumentation::reset() noexcept
	{
		m_outerHostCallbackCount.store(0, std::memory_order_relaxed);
		m_outerHostCallbackNanoseconds.store(0, std::memory_order_relaxed);
		m_outerHostCallbackMaxNanoseconds.store(0, std::memory_order_relaxed);
		m_outerHostCallbackOverrunCount.store(0, std::memory_order_relaxed);
		m_outerHostCallbackMaxOverrunNanoseconds.store(0, std::memory_order_relaxed);
		m_callbacksWithJitCompilation.store(0, std::memory_order_relaxed);
		m_bypassedCallbackCount.store(0, std::memory_order_relaxed);
		m_synthProcessCount.store(0, std::memory_order_relaxed);
		m_synthProcessNanoseconds.store(0, std::memory_order_relaxed);
		m_synthProcessMaxNanoseconds.store(0, std::memory_order_relaxed);
		m_synthProcessLockWaitNanoseconds.store(0, std::memory_order_relaxed);
		m_synthProcessLockWaitMaxNanoseconds.store(0, std::memory_order_relaxed);
		m_resamplerCallCount.store(0, std::memory_order_relaxed);
		m_resamplerNanoseconds.store(0, std::memory_order_relaxed);
		m_resamplerMaxNanoseconds.store(0, std::memory_order_relaxed);
		m_resamplerHostFrames.store(0, std::memory_order_relaxed);
		m_resamplingActiveCallbackCount.store(0, std::memory_order_relaxed);
		m_deviceProcessNanoseconds.store(0, std::memory_order_relaxed);
		m_deviceProcessMaxNanoseconds.store(0, std::memory_order_relaxed);
		m_jitCompilationCount.store(0, std::memory_order_relaxed);
		m_liveJitCompilationCount.store(0, std::memory_order_relaxed);
		m_deferredCandidateJitCompilationCount.store(0, std::memory_order_relaxed);
		m_deferredDualMachineCallbackCount.store(0, std::memory_order_relaxed);
		m_deferredCandidateAdvanceCount.store(0, std::memory_order_relaxed);
		m_deferredCandidateFrames.store(0, std::memory_order_relaxed);
		m_deferredCandidateNanoseconds.store(0, std::memory_order_relaxed);
		m_deferredCandidateMaxNanoseconds.store(0, std::memory_order_relaxed);
		m_callbacksWithAuxOutputBuses.store(0, std::memory_order_relaxed);
		m_latestActiveOutputBuses.store(0, std::memory_order_relaxed);
		m_maximumActiveOutputBuses.store(0, std::memory_order_relaxed);
		m_latestActiveOutputChannels.store(0, std::memory_order_relaxed);
		m_maximumActiveOutputChannels.store(0, std::memory_order_relaxed);
	}

	RealtimeInstrumentationSnapshot RealtimeInstrumentation::snapshot() const noexcept
	{
		RealtimeInstrumentationSnapshot result;
		result.enabled = isEnabled();
		result.outerHostCallbackCount = m_outerHostCallbackCount.load(std::memory_order_relaxed);
		result.outerHostCallbackNanoseconds = m_outerHostCallbackNanoseconds.load(std::memory_order_relaxed);
		result.outerHostCallbackMaxNanoseconds = m_outerHostCallbackMaxNanoseconds.load(std::memory_order_relaxed);
		result.outerHostCallbackOverrunCount = m_outerHostCallbackOverrunCount.load(std::memory_order_relaxed);
		result.outerHostCallbackMaxOverrunNanoseconds = m_outerHostCallbackMaxOverrunNanoseconds.load(std::memory_order_relaxed);
		result.callbacksWithJitCompilation = m_callbacksWithJitCompilation.load(std::memory_order_relaxed);
		result.bypassedCallbackCount = m_bypassedCallbackCount.load(std::memory_order_relaxed);
		result.synthProcessCount = m_synthProcessCount.load(std::memory_order_relaxed);
		result.synthProcessNanoseconds = m_synthProcessNanoseconds.load(std::memory_order_relaxed);
		result.synthProcessMaxNanoseconds = m_synthProcessMaxNanoseconds.load(std::memory_order_relaxed);
		result.synthProcessLockWaitNanoseconds = m_synthProcessLockWaitNanoseconds.load(std::memory_order_relaxed);
		result.synthProcessLockWaitMaxNanoseconds = m_synthProcessLockWaitMaxNanoseconds.load(std::memory_order_relaxed);
		result.resamplerCallCount = m_resamplerCallCount.load(std::memory_order_relaxed);
		result.resamplerNanoseconds = m_resamplerNanoseconds.load(std::memory_order_relaxed);
		result.resamplerMaxNanoseconds = m_resamplerMaxNanoseconds.load(std::memory_order_relaxed);
		result.resamplerHostFrames = m_resamplerHostFrames.load(std::memory_order_relaxed);
		result.resamplingActiveCallbackCount = m_resamplingActiveCallbackCount.load(std::memory_order_relaxed);
		result.deviceProcessNanoseconds = m_deviceProcessNanoseconds.load(std::memory_order_relaxed);
		result.deviceProcessMaxNanoseconds = m_deviceProcessMaxNanoseconds.load(std::memory_order_relaxed);
		result.jitCompilationCount = m_jitCompilationCount.load(std::memory_order_relaxed);
		result.liveJitCompilationCount = m_liveJitCompilationCount.load(std::memory_order_relaxed);
		result.deferredCandidateJitCompilationCount = m_deferredCandidateJitCompilationCount.load(std::memory_order_relaxed);
		result.deferredDualMachineCallbackCount = m_deferredDualMachineCallbackCount.load(std::memory_order_relaxed);
		result.deferredCandidateAdvanceCount = m_deferredCandidateAdvanceCount.load(std::memory_order_relaxed);
		result.deferredCandidateFrames = m_deferredCandidateFrames.load(std::memory_order_relaxed);
		result.deferredCandidateNanoseconds = m_deferredCandidateNanoseconds.load(std::memory_order_relaxed);
		result.deferredCandidateMaxNanoseconds = m_deferredCandidateMaxNanoseconds.load(std::memory_order_relaxed);
		result.callbacksWithAuxOutputBuses = m_callbacksWithAuxOutputBuses.load(std::memory_order_relaxed);
		result.latestActiveOutputBuses = m_latestActiveOutputBuses.load(std::memory_order_relaxed);
		result.maximumActiveOutputBuses = m_maximumActiveOutputBuses.load(std::memory_order_relaxed);
		result.latestActiveOutputChannels = m_latestActiveOutputChannels.load(std::memory_order_relaxed);
		result.maximumActiveOutputChannels = m_maximumActiveOutputChannels.load(std::memory_order_relaxed);
		return result;
	}

	RealtimeInstrumentation::CallbackScope::CallbackScope(
		RealtimeInstrumentation& _owner, const size_t _frames,
		const double _sampleRate, const bool _bypassed) noexcept
		: m_bypassed(_bypassed)
	{
		if(!_owner.isEnabled() || g_callbackContext.owner != nullptr)
			return;
		m_owner = &_owner;
		m_startNanoseconds = nowNanoseconds();
		if(_sampleRate > 0.0)
			m_budgetNanoseconds = static_cast<uint64_t>(std::llround(
				static_cast<double>(_frames) * 1'000'000'000.0 / _sampleRate));
		g_callbackContext = {&_owner, 0, false, false};
	}

	RealtimeInstrumentation::CallbackScope::~CallbackScope()
	{
		if(!m_owner)
			return;
		const auto duration = nowNanoseconds() - m_startNanoseconds;
		const auto jitCompilations = g_callbackContext.jitCompilations;
		const auto dualMachine = g_callbackContext.dualMachine;
		g_callbackContext = {};
		m_owner->recordHostCallback(duration, m_budgetNanoseconds,
			jitCompilations, dualMachine, m_bypassed, m_activeOutputBuses,
			m_activeOutputChannels);
	}

	void RealtimeInstrumentation::CallbackScope::setActiveOutputLayout(
		const uint32_t _buses, const uint32_t _channels) noexcept
	{
		m_activeOutputBuses = _buses;
		m_activeOutputChannels = _channels;
	}

	RealtimeInstrumentation::DeferredCandidateScope::DeferredCandidateScope(
		const uint32_t _frames) noexcept
		: m_frames(_frames)
	{
		if(!g_callbackContext.owner || !g_callbackContext.owner->isEnabled())
			return;
		m_owner = g_callbackContext.owner;
		m_previousCandidateRole = g_callbackContext.candidateRole;
		g_callbackContext.dualMachine = true;
		g_callbackContext.candidateRole = true;
		m_startNanoseconds = nowNanoseconds();
	}

	RealtimeInstrumentation::DeferredCandidateScope::~DeferredCandidateScope()
	{
		if(!m_owner)
			return;
		const auto duration = nowNanoseconds() - m_startNanoseconds;
		g_callbackContext.candidateRole = m_previousCandidateRole;
		m_owner->recordDeferredCandidate(m_frames, duration);
	}

	void RealtimeInstrumentation::recordHostCallback(
		const uint64_t _durationNanoseconds, const uint64_t _budgetNanoseconds,
		const uint64_t _jitCompilations, const bool _dualMachine,
		const bool _bypassed, const uint32_t _outputBuses,
		const uint32_t _outputChannels) noexcept
	{
		m_outerHostCallbackCount.fetch_add(1, std::memory_order_relaxed);
		m_outerHostCallbackNanoseconds.fetch_add(_durationNanoseconds, std::memory_order_relaxed);
		updateMaximum(m_outerHostCallbackMaxNanoseconds, _durationNanoseconds);
		if(_budgetNanoseconds > 0 && _durationNanoseconds > _budgetNanoseconds)
		{
			m_outerHostCallbackOverrunCount.fetch_add(1, std::memory_order_relaxed);
			updateMaximum(m_outerHostCallbackMaxOverrunNanoseconds,
				_durationNanoseconds - _budgetNanoseconds);
		}
		if(_jitCompilations > 0)
			m_callbacksWithJitCompilation.fetch_add(1, std::memory_order_relaxed);
		if(_dualMachine)
			m_deferredDualMachineCallbackCount.fetch_add(1, std::memory_order_relaxed);
		if(_bypassed)
			m_bypassedCallbackCount.fetch_add(1, std::memory_order_relaxed);
		if(_outputBuses > 1)
			m_callbacksWithAuxOutputBuses.fetch_add(1, std::memory_order_relaxed);
		m_latestActiveOutputBuses.store(_outputBuses, std::memory_order_relaxed);
		m_latestActiveOutputChannels.store(_outputChannels, std::memory_order_relaxed);
		updateMaximum(m_maximumActiveOutputBuses, _outputBuses);
		updateMaximum(m_maximumActiveOutputChannels, _outputChannels);
	}

	void RealtimeInstrumentation::recordSynthProcess(
		const uint64_t _nanoseconds) noexcept
	{
		if(!isEnabled())
			return;
		m_synthProcessCount.fetch_add(1, std::memory_order_relaxed);
		m_synthProcessNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		updateMaximum(m_synthProcessMaxNanoseconds, _nanoseconds);
	}

	void RealtimeInstrumentation::recordSynthProcessLockWait(
		const uint64_t _nanoseconds) noexcept
	{
		if(!isEnabled())
			return;
		m_synthProcessLockWaitNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		updateMaximum(m_synthProcessLockWaitMaxNanoseconds, _nanoseconds);
	}

	void RealtimeInstrumentation::recordResampler(const uint64_t _nanoseconds,
		const uint64_t _deviceNanoseconds, const size_t _hostFrames,
		const bool _resamplingActive) noexcept
	{
		if(!isEnabled())
			return;
		m_resamplerCallCount.fetch_add(1, std::memory_order_relaxed);
		m_resamplerNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		m_resamplerHostFrames.fetch_add(_hostFrames, std::memory_order_relaxed);
		m_deviceProcessNanoseconds.fetch_add(_deviceNanoseconds, std::memory_order_relaxed);
		updateMaximum(m_resamplerMaxNanoseconds, _nanoseconds);
		updateMaximum(m_deviceProcessMaxNanoseconds, _deviceNanoseconds);
		if(_resamplingActive)
			m_resamplingActiveCallbackCount.fetch_add(1, std::memory_order_relaxed);
	}

	void RealtimeInstrumentation::recordCurrentCallbackJitCompilation() noexcept
	{
		auto* const owner = g_callbackContext.owner;
		if(!owner || !owner->isEnabled())
			return;
		++g_callbackContext.jitCompilations;
		owner->m_jitCompilationCount.fetch_add(1, std::memory_order_relaxed);
		if(g_callbackContext.candidateRole)
			owner->m_deferredCandidateJitCompilationCount.fetch_add(
				1, std::memory_order_relaxed);
		else
			owner->m_liveJitCompilationCount.fetch_add(1, std::memory_order_relaxed);
	}

	void RealtimeInstrumentation::recordDeferredCandidate(
		const uint32_t _frames, const uint64_t _nanoseconds) noexcept
	{
		m_deferredCandidateAdvanceCount.fetch_add(1, std::memory_order_relaxed);
		m_deferredCandidateFrames.fetch_add(_frames, std::memory_order_relaxed);
		m_deferredCandidateNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		updateMaximum(m_deferredCandidateMaxNanoseconds, _nanoseconds);
	}
}
