#include "plugin.h"
#include "device.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "baseLib/os.h"

using namespace synthLib;

namespace synthLib
{
	constexpr uint8_t g_stateVersion = 1;

	namespace
	{
		using DiagnosticClock = std::chrono::steady_clock;

		uint64_t diagnosticNanoseconds(const DiagnosticClock::duration _duration)
		{
			return static_cast<uint64_t>(std::chrono::duration_cast<
				std::chrono::nanoseconds>(_duration).count());
		}

		void recordMaximum(std::atomic<uint64_t>& _maximum, const uint64_t _value)
		{
			auto current = _maximum.load(std::memory_order_relaxed);
			while(current < _value && !_maximum.compare_exchange_weak(current, _value,
				std::memory_order_relaxed, std::memory_order_relaxed))
			{
			}
		}

		void recordMinimum(std::atomic<uint64_t>& _minimum, const uint64_t _value)
		{
			auto current = _minimum.load(std::memory_order_relaxed);
			while(current > _value && !_minimum.compare_exchange_weak(current, _value,
				std::memory_order_relaxed, std::memory_order_relaxed))
			{
			}
		}
	}

	Plugin::Plugin(Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid)
	: m_resampler(_device->getChannelCountIn(), _device->getChannelCountOut())
	, m_device(_device)
	, m_midiClock(*this)
	, m_deviceSamplerate(_device->getSamplerate())
	, m_callbackDeviceInvalid(std::move(_callbackDeviceInvalid))
	{
	}

	void Plugin::addMidiEvent(const SMidiEvent& _ev)
	{
		std::lock_guard lock(m_lockAddMidiEvent);

		if(m_midiInRingBuffer.full())
		{
			std::lock_guard l(m_lock);
			processMidiInEvent(m_midiInRingBuffer.pop_front(), false);
		}
		m_midiInRingBuffer.push_back(_ev);
	}

	bool Plugin::setPreferredDeviceSamplerate(const float _samplerate)
	{
		std::lock_guard lock(m_lock);

		const auto sr = m_device->getDeviceSamplerate(_samplerate, m_hostSamplerate);

		if(sr == m_deviceSamplerate)  // NOLINT(clang-diagnostic-float-equal)
			return true;

		if(!m_device->setSamplerate(sr))
			return false;

		m_deviceSamplerate = sr;
		m_resampler.setSamplerates(m_hostSamplerate, m_deviceSamplerate);

		updateDeviceLatency();
		return true;
	}

	void Plugin::setHostSamplerate(const float _hostSamplerate, const float _preferredDeviceSamplerate)
	{
		std::lock_guard lock(m_lock);

		m_deviceSamplerate = m_device->getDeviceSamplerate(_preferredDeviceSamplerate, _hostSamplerate);
		m_device->setSamplerate(m_deviceSamplerate);
		m_resampler.setSamplerates(_hostSamplerate, m_deviceSamplerate);

		m_hostSamplerate = _hostSamplerate;
		m_hostSamplerateInv = _hostSamplerate > 0 ? 1.0f / _hostSamplerate : 0.0f;

		updateDeviceLatency();
	}

	void Plugin::setResamplerMode(const Resampler::Mode _mode)
	{
		std::lock_guard lock(m_lock);
		m_resampler.setResamplerMode(_mode);
		updateDeviceLatency();
	}

	void Plugin::reserveMidiEventCapacity(const size_t _capacity)
	{
		std::lock_guard lock(m_lock);
		m_midiIn.reserve(_capacity);
		m_midiOut.reserve(_capacity);
		m_resampler.reserveMidiEventCapacity(_capacity);
		m_device->reserveMidiEventCapacity(_capacity);
	}

	void Plugin::process(const TAudioInputs& _inputs, const TAudioOutputs& _outputs,
		const size_t _count, const float _bpm, const float _ppqPos,
		const bool _isPlaying)
	{
		baseLib::setFlushDenormalsToZero();
		const bool diagnosticsEnabled = m_audioDiagnosticsEnabled.load(
			std::memory_order_relaxed);
		const auto callbackStart = diagnosticsEnabled
			? DiagnosticClock::now() : DiagnosticClock::time_point{};

		TAudioInputs inputs(_inputs);
		TAudioOutputs outputs(_outputs);

		std::unique_lock lock(m_lock);
		const auto lockAcquired = diagnosticsEnabled
			? DiagnosticClock::now() : DiagnosticClock::time_point{};
		const auto diagnosticHostSamplerate = m_hostSamplerate;
		uint64_t deviceNanoseconds = 0;
		uint64_t resamplerNanoseconds = 0;
		bool invalidDevice = false;
		const auto finishDiagnostics = [&]
		{
			if(!diagnosticsEnabled)
				return;
			const auto callbackEnd = DiagnosticClock::now();
			const auto callbackNs = diagnosticNanoseconds(callbackEnd - callbackStart);
			const auto lockWaitNs = diagnosticNanoseconds(lockAcquired - callbackStart);
			m_diagnosticCallbackCount.fetch_add(1, std::memory_order_relaxed);
			m_diagnosticCallbackSamples.fetch_add(_count, std::memory_order_relaxed);
			m_diagnosticLastCallbackSamples.store(_count, std::memory_order_relaxed);
			recordMinimum(m_diagnosticMinimumCallbackSamples, _count);
			recordMaximum(m_diagnosticMaximumCallbackSamples, _count);
			if(_isPlaying)
				m_diagnosticPlayingCallbackCount.fetch_add(1,
					std::memory_order_relaxed);
			m_diagnosticCallbackNanoseconds.fetch_add(callbackNs,
				std::memory_order_relaxed);
			m_diagnosticLockWaitNanoseconds.fetch_add(lockWaitNs,
				std::memory_order_relaxed);
			m_diagnosticResamplerNanoseconds.fetch_add(resamplerNanoseconds,
				std::memory_order_relaxed);
			m_diagnosticDeviceNanoseconds.fetch_add(deviceNanoseconds,
				std::memory_order_relaxed);
			recordMaximum(m_diagnosticMaximumCallbackNanoseconds, callbackNs);
			recordMaximum(m_diagnosticMaximumLockWaitNanoseconds, lockWaitNs);
			if(invalidDevice)
				m_diagnosticInvalidDeviceCallbackCount.fetch_add(1,
					std::memory_order_relaxed);
			if(diagnosticHostSamplerate > 0.0f
				&& callbackNs > static_cast<uint64_t>(
					1.0e9 * static_cast<double>(_count) / diagnosticHostSamplerate))
				m_diagnosticDeadlineMissCount.fetch_add(1, std::memory_order_relaxed);
		};
		if(_count > m_blockSize)
			++m_realtimeAllocationFallbackCount;

		auto* const silentInput = getSilentInputBuffer(_count);
		for(size_t i=0; i<inputs.size(); ++i)
			inputs[i] = _inputs[i] ? _inputs[i] : silentInput;

		if(!m_device->isValid())
		{
			invalidDevice = true;
			m_callbackDeviceInvalid(m_device);
			lock.unlock();
			finishDiagnostics();
			return;
		}

		for(size_t i=0; i<m_device->getChannelCountOut(); ++i)
			outputs[i] = _outputs[i] ? _outputs[i]
				: getDiscardOutputBuffer(i, _count);

		processMidiInEvents();
		processMidiClock(_bpm, _ppqPos, _isPlaying, _count);

		const auto midiOutBegin = m_midiOut.size();
		const auto resamplerStart = diagnosticsEnabled
			? DiagnosticClock::now() : DiagnosticClock::time_point{};
		m_resampler.process(inputs, outputs, m_midiIn, m_midiOut,
			static_cast<uint32_t>(_count),
			[&](const TAudioInputs& _ins, const TAudioOutputs& _outs, size_t _c, const ResamplerInOut::TMidiVec& _midiIn, ResamplerInOut::TMidiVec& _midiOut)
		{
			const auto deviceStart = diagnosticsEnabled
				? DiagnosticClock::now() : DiagnosticClock::time_point{};
			m_device->process(_ins, _outs, _c, _midiIn, _midiOut);
			if(diagnosticsEnabled)
				deviceNanoseconds += diagnosticNanoseconds(
					DiagnosticClock::now() - deviceStart);
		});
		if(diagnosticsEnabled)
			resamplerNanoseconds = diagnosticNanoseconds(
				DiagnosticClock::now() - resamplerStart);
		for(size_t i = midiOutBegin; i < m_midiOut.size(); ++i)
			if(!m_midiOut[i].sysex.empty())
				++m_realtimeAllocationFallbackCount;

		m_midiIn.clear();
		lock.unlock();
		finishDiagnostics();
	}

	void Plugin::setAudioDiagnosticsEnabled(const bool _enabled)
	{
		m_audioDiagnosticsEnabled.store(_enabled, std::memory_order_relaxed);
	}

	void Plugin::recordHostAudioCallback(const size_t _count,
		const float _hostSamplerate, const uint64_t _nanoseconds)
	{
		if(!m_audioDiagnosticsEnabled.load(std::memory_order_relaxed))
			return;
		m_diagnosticHostCallbackCount.fetch_add(1, std::memory_order_relaxed);
		m_diagnosticHostCallbackNanoseconds.fetch_add(_nanoseconds,
			std::memory_order_relaxed);
		recordMaximum(m_diagnosticMaximumHostCallbackNanoseconds, _nanoseconds);
		if(_hostSamplerate > 0.0f && _nanoseconds > static_cast<uint64_t>(
			1.0e9 * static_cast<double>(_count) / _hostSamplerate))
			m_diagnosticHostDeadlineMissCount.fetch_add(1, std::memory_order_relaxed);
	}

	AudioDiagnosticsSnapshot Plugin::getAudioDiagnosticsSnapshot() const
	{
		const auto callbackCount = m_diagnosticCallbackCount.load(
			std::memory_order_relaxed);
		return {
			callbackCount,
			m_diagnosticCallbackSamples.load(std::memory_order_relaxed),
			m_diagnosticLastCallbackSamples.load(std::memory_order_relaxed),
			callbackCount ? m_diagnosticMinimumCallbackSamples.load(
				std::memory_order_relaxed) : 0,
			m_diagnosticMaximumCallbackSamples.load(std::memory_order_relaxed),
			m_diagnosticPlayingCallbackCount.load(std::memory_order_relaxed),
			m_diagnosticCallbackNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticMaximumCallbackNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticLockWaitNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticMaximumLockWaitNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticResamplerNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticDeviceNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticDeadlineMissCount.load(std::memory_order_relaxed),
			m_diagnosticInvalidDeviceCallbackCount.load(std::memory_order_relaxed),
			m_diagnosticHostCallbackCount.load(std::memory_order_relaxed),
			m_diagnosticHostCallbackNanoseconds.load(std::memory_order_relaxed),
			m_diagnosticMaximumHostCallbackNanoseconds.load(
				std::memory_order_relaxed),
			m_diagnosticHostDeadlineMissCount.load(std::memory_order_relaxed)
		};
	}

	void Plugin::resetAudioDiagnostics()
	{
		m_diagnosticCallbackCount.store(0, std::memory_order_relaxed);
		m_diagnosticCallbackSamples.store(0, std::memory_order_relaxed);
		m_diagnosticLastCallbackSamples.store(0, std::memory_order_relaxed);
		m_diagnosticMinimumCallbackSamples.store(~uint64_t{0},
			std::memory_order_relaxed);
		m_diagnosticMaximumCallbackSamples.store(0, std::memory_order_relaxed);
		m_diagnosticPlayingCallbackCount.store(0, std::memory_order_relaxed);
		m_diagnosticCallbackNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticMaximumCallbackNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticLockWaitNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticMaximumLockWaitNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticResamplerNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticDeviceNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticDeadlineMissCount.store(0, std::memory_order_relaxed);
		m_diagnosticInvalidDeviceCallbackCount.store(0, std::memory_order_relaxed);
		m_diagnosticHostCallbackCount.store(0, std::memory_order_relaxed);
		m_diagnosticHostCallbackNanoseconds.store(0, std::memory_order_relaxed);
		m_diagnosticMaximumHostCallbackNanoseconds.store(0,
			std::memory_order_relaxed);
		m_diagnosticHostDeadlineMissCount.store(0, std::memory_order_relaxed);
	}

	void Plugin::getMidiOut(std::vector<SMidiEvent>& _midiOut)
	{
		std::swap(_midiOut, m_midiOut);
		m_midiOut.clear();
	}

	bool Plugin::isValid() const
	{
		return m_device->isValid();
	}

	void Plugin::setDevice(Device* _device)
	{
		if(!_device)
			return;

		std::lock_guard lock(m_lock);

		std::vector<uint8_t> deviceState;
		getState(deviceState, StateTypeGlobal);

		delete m_device;

		m_device = _device;

		configureDeviceAudio();
		if(!deviceState.empty())
			setState(deviceState);

		// MIDI clock has to send the start event again, some device find it confusing and do strange things if there isn't any
		m_midiClock.restart();

	}

#if !SYNTHLIB_DEMO_MODE
	bool Plugin::getState(std::vector<uint8_t>& _state, StateType _type) const
	{
		std::lock_guard lock(m_lock);

		if(!m_device)
			return false;

		_state.push_back(g_stateVersion);
		_state.push_back(_type);

		return m_device->getState(_state, _type);
	}

	bool Plugin::setState(const std::vector<uint8_t>& _state) const
	{
		if(_state.empty())
			return false;

		if(_state.size() < 2)
		{
			std::lock_guard lock(m_lock);
			return m_device && m_device->setStateFromUnknownCustomData(_state);
		}

		const auto version = _state[0];

		if(version != g_stateVersion)
		{
			std::lock_guard lock(m_lock);
			return m_device && m_device->setStateFromUnknownCustomData(_state);
		}

		const auto stateType = static_cast<StateType>(_state[1]);

		auto state = _state;
		state.erase(state.begin(), state.begin() + 2);
		auto transactionState =
			std::make_shared<const std::vector<uint8_t>>(std::move(state));

		Device* transactionDevice = nullptr;
		std::unique_ptr<Device::StateTransaction> transaction;
		{
			std::lock_guard lock(m_lock);
			if(!m_device)
				return false;
			if(!m_device->supportsStateTransactions())
				return m_device->setState(*transactionState, stateType);
			transactionDevice = m_device;
			transaction = m_device->beginStateTransaction(
				std::move(transactionState), stateType);
		}
		if(!transaction)
			return false;

		const auto preparationSucceeded = transaction->prepare();
		bool finished = false;
		{
			std::lock_guard lock(m_lock);
			if(m_device == transactionDevice)
				finished = m_device->finishStateTransaction(*transaction);
		}
		// The transaction may own a replaced Device implementation. Its destructor
		// deliberately runs after the process/device lock has been released.
		return preparationSucceeded && finished;
	}
#endif
	void Plugin::insertMidiEvent(const SMidiEvent& _ev)
	{
		if(m_midiIn.empty() || m_midiIn.back().offset <= _ev.offset)
		{
			m_midiIn.push_back(_ev);
			return;
		}

		for (auto it = m_midiIn.begin(); it != m_midiIn.end(); ++it)
		{
			if (it->offset > _ev.offset)
			{
				m_midiIn.insert(it, _ev);
				return;
			}
		}

		m_midiIn.push_back(_ev);
	}

	bool Plugin::setLatencyBlocks(uint32_t _latencyBlocks)
	{
		std::lock_guard lock(m_lock);

		if(m_extraLatencyBlocks == _latencyBlocks)
			return false;

		m_extraLatencyBlocks = _latencyBlocks;
		updateDeviceLatency();
		return true;
	}

	void Plugin::processMidiClock(const float _bpm, const float _ppqPos, const bool _isPlaying, const size_t _sampleCount)
	{
		m_midiClock.process(_bpm, _ppqPos, _isPlaying, _sampleCount);
	}

	float* Plugin::getSilentInputBuffer(const size_t _minimumSize)
	{
		if(m_silentInputBuffer.size() < _minimumSize)
			m_silentInputBuffer.resize(_minimumSize);
		std::fill_n(m_silentInputBuffer.data(), _minimumSize, 0.0f);
		return m_silentInputBuffer.data();
	}

	float* Plugin::getDiscardOutputBuffer(const size_t _channel,
		const size_t _minimumSize)
	{
		auto& buffer = m_discardOutputBuffers[_channel];
		if(buffer.size() < _minimumSize)
			buffer.resize(_minimumSize);
		return buffer.data();
	}

	void Plugin::configureDeviceAudio()
	{
		m_device->setSamplerate(m_deviceSamplerate);
		m_resampler.reconfigure(m_device->getChannelCountIn(),
			m_device->getChannelCountOut(), m_hostSamplerate, m_deviceSamplerate);
		for(size_t channel = 0; channel < m_device->getChannelCountOut(); ++channel)
			m_discardOutputBuffers[channel].resize(m_blockSize);
		updateDeviceLatency();
	}

	void Plugin::updateDeviceLatency()
	{
		if(m_blockSize <= 0 || m_hostSamplerate <= 0)
			return;

		const auto latency = static_cast<uint32_t>(std::ceil(static_cast<float>(m_blockSize * m_extraLatencyBlocks) * m_device->getSamplerate() * m_hostSamplerateInv));
		m_device->setExtraLatencySamples(latency);
		m_deviceExtraLatencyHost = static_cast<uint32_t>(std::ceil(
			static_cast<float>(m_device->getExtraLatencySamples())
			* m_hostSamplerate / m_device->getSamplerate()));

		m_deviceLatencyMidiToOutput = static_cast<uint32_t>(static_cast<float>(m_device->getInternalLatencyMidiToOutput()) * m_hostSamplerate / m_device->getSamplerate());
		m_deviceLatencyInputToOutput = static_cast<uint32_t>(static_cast<float>(m_device->getInternalLatencyInputToOutput()) * m_hostSamplerate / m_device->getSamplerate());
	}

	void Plugin::processMidiInEvents()
	{
		while (!m_midiInRingBuffer.empty())
		{
			const auto ev = m_midiInRingBuffer.pop_front();

			processMidiInEvent(ev, true);
		}
	}

	void Plugin::processMidiInEvent(const SMidiEvent& _ev, const bool _realtime)
	{
		// sysex might be sent in multiple chunks. Happens if coming from hardware
		if (!_ev.sysex.empty())
		{
			if(_realtime)
				++m_realtimeAllocationFallbackCount;
			const bool isComplete = _ev.sysex.front() == M_STARTOFSYSEX && _ev.sysex.back() == M_ENDOFSYSEX;

			if (isComplete)
			{
				m_midiIn.push_back(_ev);
				return;
			}

			const bool isStart = _ev.sysex.front() == M_STARTOFSYSEX && _ev.sysex.back() != M_ENDOFSYSEX;
			const bool isEnd = _ev.sysex.front() != M_STARTOFSYSEX && _ev.sysex.back() == M_ENDOFSYSEX;

			if (isStart)
			{
				m_pendingSysexInput = _ev;
				return;
			}

			if (!m_pendingSysexInput.sysex.empty())
			{
				m_pendingSysexInput.sysex.insert(m_pendingSysexInput.sysex.end(), _ev.sysex.begin(), _ev.sysex.end());

				if (isEnd)
				{
					m_midiIn.push_back(m_pendingSysexInput);
					m_pendingSysexInput.sysex.clear();
				}
			}
		}

		m_midiIn.push_back(_ev);
	}

	void Plugin::setBlockSize(const uint32_t _blockSize)
	{
		std::lock_guard lock(m_lock);
		m_blockSize = _blockSize;
		m_silentInputBuffer.resize(_blockSize);
		for(size_t channel = 0; channel < m_device->getChannelCountOut(); ++channel)
			m_discardOutputBuffers[channel].resize(_blockSize);
		m_resampler.prepare(_blockSize);
		updateDeviceLatency();
	}

	uint32_t Plugin::getLatencyMidiToOutput() const
	{
		std::lock_guard lock(m_lock);
		return m_deviceExtraLatencyHost + m_deviceLatencyMidiToOutput
			+ m_resampler.getOutputLatency();
	}

	uint32_t Plugin::getLatencyInputToOutput() const
	{
		std::lock_guard lock(m_lock);
		return m_deviceExtraLatencyHost + m_deviceLatencyInputToOutput
			+ m_resampler.getOutputLatency() + m_resampler.getInputLatency();
	}
}
