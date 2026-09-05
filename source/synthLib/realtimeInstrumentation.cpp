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
		synthLib::RealtimeSlowCallback callback;
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
		m_captureEpoch.fetch_add(1, std::memory_order_relaxed);
		m_timelineEvents.store(0, std::memory_order_relaxed);
		m_timelineEventsDropped.store(0, std::memory_order_relaxed);
		m_offlineCallbackCount.store(0, std::memory_order_relaxed);
		m_slowCallbacksDropped.store(0, std::memory_order_relaxed);
		for(auto& bucket : m_realtimeBudgetHistogram) bucket.store(0, std::memory_order_relaxed);
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
		result.timelineEvents = m_timelineEvents.load(std::memory_order_relaxed);
		result.timelineEventsDropped = m_timelineEventsDropped.load(std::memory_order_relaxed);
		result.offlineCallbackCount = m_offlineCallbackCount.load(std::memory_order_relaxed);
		result.slowCallbacksDropped = m_slowCallbacksDropped.load(std::memory_order_relaxed);
		for(size_t i = 0; i < result.realtimeBudgetHistogram.size(); ++i)
			result.realtimeBudgetHistogram[i] = m_realtimeBudgetHistogram[i].load(std::memory_order_relaxed);
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
	{
		if(!_owner.isEnabled() || g_callbackContext.owner != nullptr)
			return;
		m_owner = &_owner;
		m_startNanoseconds = nowNanoseconds();
		if(std::isfinite(_sampleRate) && _sampleRate > 0.0)
			m_budgetNanoseconds = static_cast<uint64_t>(std::llround(
				static_cast<double>(_frames) * 1'000'000'000.0 / _sampleRate));
		m_bypassed = _bypassed;
		m_frames = static_cast<uint32_t>(_frames);
		m_sampleRate = std::isfinite(_sampleRate) && _sampleRate > 0.0 ? _sampleRate : 0;
		g_callbackContext = {};
		g_callbackContext.owner = &_owner;
	}

	RealtimeInstrumentation::CallbackScope::~CallbackScope()
	{
		if(!m_owner)
			return;
		const auto duration = nowNanoseconds() - m_startNanoseconds;
		auto callback = g_callbackContext.callback;
		callback.startNanoseconds = m_startNanoseconds;
		callback.durationNanoseconds = duration;
		callback.budgetNanoseconds = m_budgetNanoseconds;
		callback.frames = m_frames;
		callback.sampleRate = m_sampleRate;
		callback.bypassed = m_bypassed;
		callback.playing = m_playing;
		callback.offline = m_offline;
		callback.midiEvents = m_midiEvents;
		callback.midiBytes = m_midiBytes;
		callback.outputBuses = m_activeOutputBuses;
		callback.outputChannels = m_activeOutputChannels;
		callback.dualMachine = g_callbackContext.dualMachine;
		g_callbackContext = {};
		m_owner->recordHostCallback(callback);
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

	void RealtimeInstrumentation::recordHostCallback(RealtimeSlowCallback _callback) noexcept
	{
		_callback.index = m_outerHostCallbackCount.fetch_add(1, std::memory_order_relaxed) + 1;
		m_outerHostCallbackNanoseconds.fetch_add(_callback.durationNanoseconds, std::memory_order_relaxed);
		updateMaximum(m_outerHostCallbackMaxNanoseconds, _callback.durationNanoseconds);
		if(!_callback.offline && _callback.budgetNanoseconds > 0 && _callback.durationNanoseconds > _callback.budgetNanoseconds)
		{
			m_outerHostCallbackOverrunCount.fetch_add(1, std::memory_order_relaxed);
			updateMaximum(m_outerHostCallbackMaxOverrunNanoseconds,
				_callback.durationNanoseconds - _callback.budgetNanoseconds);
		}
		if((_callback.liveJitCompilations + _callback.deferredJitCompilations) > 0)
			m_callbacksWithJitCompilation.fetch_add(1, std::memory_order_relaxed);
		if(_callback.dualMachine)
			m_deferredDualMachineCallbackCount.fetch_add(1, std::memory_order_relaxed);
		if(_callback.bypassed)
			m_bypassedCallbackCount.fetch_add(1, std::memory_order_relaxed);
		if(_callback.outputBuses > 1)
			m_callbacksWithAuxOutputBuses.fetch_add(1, std::memory_order_relaxed);
		m_latestActiveOutputBuses.store(_callback.outputBuses, std::memory_order_relaxed);
		m_latestActiveOutputChannels.store(_callback.outputChannels, std::memory_order_relaxed);
		updateMaximum(m_maximumActiveOutputBuses, _callback.outputBuses);
		updateMaximum(m_maximumActiveOutputChannels, _callback.outputChannels);
		if(_callback.offline)
			m_offlineCallbackCount.fetch_add(1, std::memory_order_relaxed);
		else if(_callback.budgetNanoseconds > 0)
		{
			const auto ratio = static_cast<double>(_callback.durationNanoseconds) / _callback.budgetNanoseconds;
			const size_t bucket = ratio < .25 ? 0 : ratio < .5 ? 1 : ratio < .75 ? 2 : ratio < 1 ? 3 : ratio < 1.5 ? 4 : 5;
			m_realtimeBudgetHistogram[bucket].fetch_add(1, std::memory_order_relaxed);
		}
		_callback.interCallbackNanoseconds = m_previousCallbackStart > 0
			? _callback.startNanoseconds - m_previousCallbackStart : 0;
		m_previousCallbackStart = _callback.startNanoseconds;
		// Include occasional ordinary callbacks for workload context. Trace near
		// deadlines, significant waits, compilation and dual-machine processing.
		const bool interesting = _callback.index == 1 || _callback.index % 1024 == 0
			|| (!_callback.offline && _callback.budgetNanoseconds > 0
				&& _callback.durationNanoseconds >= _callback.budgetNanoseconds * 3 / 4)
			|| _callback.lockWaitNanoseconds >= 100'000 || _callback.dualMachine
			|| _callback.liveJitCompilations || _callback.deferredJitCompilations;
		if(!interesting) return;
		auto& slot = m_slowCallbacks[m_writePosition % SlowCallbackCapacity];
		if(slot.ready.load(std::memory_order_acquire))
		{
			m_slowCallbacksDropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		slot.callback = _callback;
		slot.ready.store(true, std::memory_order_release);
		++m_writePosition;
	}

	bool RealtimeInstrumentation::popSlowCallback(RealtimeSlowCallback& _callback) noexcept
	{
		auto& slot = m_slowCallbacks[m_readPosition % SlowCallbackCapacity];
		if(!slot.ready.load(std::memory_order_acquire)) return false;
		_callback = slot.callback;
		slot.ready.store(false, std::memory_order_release);
		++m_readPosition;
		return true;
	}

	void RealtimeInstrumentation::CallbackScope::setHostState(bool _playing, bool _offline, bool _transportKnown) noexcept
	{
		m_playing = _playing;
		m_offline = _offline;
		if(m_owner) m_owner->recordHostTransport(_playing, _offline, m_bypassed, _transportKnown);
	}

	bool RealtimeInstrumentation::recordTimelineEvent(RealtimeEvent _event) noexcept
	{
		if(!isEnabled()) return false;
		if(!_event.timeNanoseconds) _event.timeNanoseconds = nowNanoseconds();
		_event.sequence = m_timelineEvents.fetch_add(1, std::memory_order_relaxed) + 1;
		if(m_eventProducer.test_and_set(std::memory_order_acquire))
		{
			m_timelineEventsDropped.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		auto& slot = m_timeline[m_eventWritePosition % TimelineCapacity];
		const bool available = !slot.ready.load(std::memory_order_acquire);
		if(!available)
			m_timelineEventsDropped.fetch_add(1, std::memory_order_relaxed);
		else
		{
			slot.event = _event;
			slot.ready.store(true, std::memory_order_release);
			++m_eventWritePosition;
		}
		m_eventProducer.clear(std::memory_order_release);
		return available;
	}

	bool RealtimeInstrumentation::popTimelineEvent(RealtimeEvent& _event) noexcept
	{
		auto& slot = m_timeline[m_eventReadPosition % TimelineCapacity];
		if(!slot.ready.load(std::memory_order_acquire)) return false;
		_event = slot.event;
		slot.ready.store(false, std::memory_order_release);
		++m_eventReadPosition;
		return true;
	}

	RealtimeInstrumentation::PanelInputToken RealtimeInstrumentation::beginPanelInput(
		uint32_t _model, uint8_t _command, uint8_t _argument) noexcept
	{
		if(!isEnabled()) return {};
		PanelInputToken token{m_nextInputId.fetch_add(1, std::memory_order_relaxed),
			m_captureEpoch.load(std::memory_order_relaxed)};
		RealtimeEvent event;
		event.kind = RealtimeEventKind::PanelInput;
		event.inputId = token.id;
		event.model = _model; event.command = _command; event.argument = _argument;
		recordTimelineEvent(event);
		return token;
	}

	void RealtimeInstrumentation::endPanelInput(PanelInputToken _token, uint32_t _model,
		uint8_t _command, uint8_t _argument, bool _accepted) noexcept
	{
		if(!_token.id || _token.epoch != m_captureEpoch.load(std::memory_order_relaxed)) return;
		RealtimeEvent event;
		event.kind = RealtimeEventKind::PanelInputResult;
		event.inputId = _token.id;
		event.model = _model; event.command = _command; event.argument = _argument;
		event.accepted = _accepted;
		recordTimelineEvent(event);
	}

	void RealtimeInstrumentation::recordCurrentPanelDelivery(uint32_t _model,
		uint8_t _command, uint8_t _argument) noexcept
	{
		auto* owner = g_callbackContext.owner;
		if(!owner || !owner->isEnabled()) return;
		RealtimeEvent event;
		event.kind = RealtimeEventKind::PanelDelivery;
		event.model = _model; event.command = _command; event.argument = _argument;
		event.callbackIndex = owner->m_outerHostCallbackCount.load(std::memory_order_relaxed) + 1;
		event.deferred = g_callbackContext.candidateRole;
		owner->recordTimelineEvent(event);
	}

	void RealtimeInstrumentation::recordHostTransport(bool _playing, bool _offline,
		bool _bypassed, bool _known) noexcept
	{
		if(!isEnabled()) return;
		const auto epoch = m_captureEpoch.load(std::memory_order_relaxed);
		const uint32_t flags = (_playing ? 1 : 0) | (_offline ? 2 : 0)
			| (_bypassed ? 4 : 0) | (_known ? 8 : 0);
		if(epoch == m_transportEpoch && flags == m_lastTransportFlags) return;
		const bool initial = epoch != m_transportEpoch;
		RealtimeEvent event;
		event.kind = RealtimeEventKind::HostTransport;
		event.callbackIndex = m_outerHostCallbackCount.load(std::memory_order_relaxed) + 1;
		event.playing = _playing; event.offline = _offline; event.bypassed = _bypassed;
		event.transportKnown = _known;
		event.initial = initial;
		// Retry the latest state on the next callback if this attempt was dropped.
		if(recordTimelineEvent(event))
		{
			m_transportEpoch = epoch;
			m_lastTransportFlags = flags;
		}
	}

	void RealtimeInstrumentation::CallbackScope::setMidiInputSummary(uint32_t _events, uint32_t _bytes) noexcept
	{
		m_midiEvents = _events;
		m_midiBytes = _bytes;
	}

	void RealtimeInstrumentation::setCurrentDeviceContext(uint32_t _rate, uint32_t _mode, uint32_t _clockPercent) noexcept
	{
		if(!g_callbackContext.owner) return;
		g_callbackContext.callback.deviceSampleRate = _rate;
		g_callbackContext.callback.resamplerMode = _mode;
		g_callbackContext.callback.dspClockPercent = _clockPercent;
	}

	void RealtimeInstrumentation::recordSynthProcess(
		const uint64_t _nanoseconds) noexcept
	{
		if(!isEnabled())
			return;
		if(g_callbackContext.owner == this) g_callbackContext.callback.synthNanoseconds += _nanoseconds;
		m_synthProcessCount.fetch_add(1, std::memory_order_relaxed);
		m_synthProcessNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		updateMaximum(m_synthProcessMaxNanoseconds, _nanoseconds);
	}

	void RealtimeInstrumentation::recordSynthProcessLockWait(
		const uint64_t _nanoseconds) noexcept
	{
		if(!isEnabled())
			return;
		if(g_callbackContext.owner == this) g_callbackContext.callback.lockWaitNanoseconds += _nanoseconds;
		m_synthProcessLockWaitNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		updateMaximum(m_synthProcessLockWaitMaxNanoseconds, _nanoseconds);
	}

	void RealtimeInstrumentation::recordResampler(const uint64_t _nanoseconds,
		const uint64_t _deviceNanoseconds, const size_t _hostFrames,
		const bool _resamplingActive) noexcept
	{
		if(!isEnabled())
			return;
		if(g_callbackContext.owner == this)
		{
			g_callbackContext.callback.resamplerNanoseconds += _nanoseconds;
			g_callbackContext.callback.deviceNanoseconds += _deviceNanoseconds;
			g_callbackContext.callback.resamplingActive |= _resamplingActive;
		}
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
		if(g_callbackContext.candidateRole) ++g_callbackContext.callback.deferredJitCompilations;
		else ++g_callbackContext.callback.liveJitCompilations;
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
		if(g_callbackContext.owner == this) g_callbackContext.callback.deferredNanoseconds += _nanoseconds;
		m_deferredCandidateAdvanceCount.fetch_add(1, std::memory_order_relaxed);
		m_deferredCandidateFrames.fetch_add(_frames, std::memory_order_relaxed);
		m_deferredCandidateNanoseconds.fetch_add(_nanoseconds, std::memory_order_relaxed);
		updateMaximum(m_deferredCandidateMaxNanoseconds, _nanoseconds);
	}
}
