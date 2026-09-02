#pragma once

#include <array>
#include <mutex>
#include <atomic>
#include <functional>
#include <tuple>
#include <utility>

#include "midiTypes.h"
#include "resamplerInOut.h"
#include "buildconfig.h"

#include "dsp56kBase/ringbuffer.h"

#include "deviceTypes.h"
#include "midiClock.h"

namespace synthLib
{
	class Device;

	struct AudioDiagnosticsSnapshot
	{
		uint64_t callbackCount = 0;
		uint64_t callbackSamples = 0;
		uint64_t lastCallbackSamples = 0;
		uint64_t minimumCallbackSamples = 0;
		uint64_t maximumCallbackSamples = 0;
		uint64_t playingCallbackCount = 0;
		uint64_t callbackNanoseconds = 0;
		uint64_t maximumCallbackNanoseconds = 0;
		uint64_t lockWaitNanoseconds = 0;
		uint64_t maximumLockWaitNanoseconds = 0;
		uint64_t resamplerNanoseconds = 0;
		uint64_t deviceNanoseconds = 0;
		uint64_t deadlineMissCount = 0;
		uint64_t invalidDeviceCallbackCount = 0;
		uint64_t hostCallbackCount = 0;
		uint64_t hostCallbackNanoseconds = 0;
		uint64_t maximumHostCallbackNanoseconds = 0;
		uint64_t hostDeadlineMissCount = 0;
	};

	class Plugin
	{
	public:
		static constexpr size_t RealtimeMidiEventCapacity = 1024;

		using CallbackDeviceInvalid = std::function<void(Device*)>;

		Plugin(Device* _device, CallbackDeviceInvalid _callbackDeviceInvalid);

		void addMidiEvent(const SMidiEvent& _ev);

		bool setPreferredDeviceSamplerate(float _samplerate);

		void setHostSamplerate(float _hostSamplerate, float _preferredDeviceSamplerate);
		void setResamplerMode(Resampler::Mode _mode);
		float getHostSamplerate() const { return m_hostSamplerate; }
		float getHostSamplerateInv() const { return m_hostSamplerateInv; }

		void setBlockSize(uint32_t _blockSize);
		void reserveMidiEventCapacity(size_t _capacity = RealtimeMidiEventCapacity);

		uint32_t getLatencyMidiToOutput() const;
		uint32_t getLatencyInputToOutput() const;
		// Counts visits to explicitly unsupported RT allocation paths: a host block
		// larger than setBlockSize, or dynamically sized SysEx processed/emitted by
		// the device. Prepared audio and channel-message MIDI must leave this stable.
		uint64_t getRealtimeAllocationFallbackCount() const
		{
			return m_realtimeAllocationFallbackCount.load();
		}
		// Disabled by default. Diagnostic builds enable this explicitly so normal
		// releases pay only for one relaxed atomic load per callback.
		void setAudioDiagnosticsEnabled(bool _enabled);
		bool isAudioDiagnosticsEnabled() const
		{
			return m_audioDiagnosticsEnabled.load(std::memory_order_relaxed);
		}
		AudioDiagnosticsSnapshot getAudioDiagnosticsSnapshot() const;
		void resetAudioDiagnostics();
		void recordHostAudioCallback(size_t _count, float _hostSamplerate,
			uint64_t _nanoseconds);

		void process(const TAudioInputs& _inputs, const TAudioOutputs& _outputs,
			size_t _count, float _bpm, float _ppqPos, bool _isPlaying);
		void getMidiOut(std::vector<SMidiEvent>& _midiOut);

		bool isValid() const;

		void setDevice(Device* _device);
		Device* getDevice() const { return m_device; }

		// Keep a short control-plane operation pinned to the current Device. Callers
		// must not perform file I/O, allocation-heavy preparation, or other long work
		// inside this callback; capture immutable context here and prepare outside.
		template<typename Callback>
		decltype(auto) withDeviceLocked(Callback&& _callback) const
		{
			std::lock_guard lock(m_lock);
			return std::forward<Callback>(_callback)(m_device);
		}

		// Diagnostic/status readers must never wait behind audio processing. Keep
		// the callback to copies of already-atomic counters.
		template<typename Callback>
		bool tryWithDeviceLocked(Callback&& _callback) const
		{
			std::unique_lock lock(m_lock, std::try_to_lock);
			if(!lock.owns_lock())
				return false;
			std::forward<Callback>(_callback)(m_device);
			return true;
		}

#if !SYNTHLIB_DEMO_MODE
		bool getState(std::vector<uint8_t>& _state, StateType _type) const;
		bool setState(const std::vector<uint8_t>& _state) const;
#endif
		void insertMidiEvent(const SMidiEvent& _ev);

		bool setLatencyBlocks(uint32_t _latencyBlocks);
		uint32_t getLatencyBlocks() const { return m_extraLatencyBlocks; }

	private:
		void processMidiClock(float _bpm, float _ppqPos, bool _isPlaying, size_t _sampleCount);
		float* getSilentInputBuffer(size_t _minimumSize);
		float* getDiscardOutputBuffer(size_t _channel, size_t _minimumSize);
		void configureDeviceAudio();
		void updateDeviceLatency();
		void processMidiInEvents();
		void processMidiInEvent(const SMidiEvent& _ev, bool _realtime);

		dsp56k::RingBuffer<SMidiEvent, RealtimeMidiEventCapacity, false> m_midiInRingBuffer;
		std::vector<SMidiEvent> m_midiIn;
		std::vector<SMidiEvent> m_midiOut;

		SMidiEvent m_pendingSysexInput;

		ResamplerInOut m_resampler;
		mutable std::recursive_mutex m_lock;
		mutable std::mutex m_lockAddMidiEvent;

		Device* m_device;

		std::vector<float> m_silentInputBuffer;
		std::array<std::vector<float>, std::tuple_size_v<TAudioOutputs>>
			m_discardOutputBuffers;

		float m_hostSamplerate = 0.0f;
		float m_hostSamplerateInv = 0.0f;

		uint32_t m_blockSize = 0;

		uint32_t m_deviceLatencyMidiToOutput = 0;
		uint32_t m_deviceLatencyInputToOutput = 0;
		uint32_t m_deviceExtraLatencyHost = 0;

		MidiClock m_midiClock;

		uint32_t m_extraLatencyBlocks = 1;

		float m_deviceSamplerate = 0.0f;
		CallbackDeviceInvalid m_callbackDeviceInvalid;
		std::atomic<uint64_t> m_realtimeAllocationFallbackCount{0};

		std::atomic<bool> m_audioDiagnosticsEnabled{false};
		std::atomic<uint64_t> m_diagnosticCallbackCount{0};
		std::atomic<uint64_t> m_diagnosticCallbackSamples{0};
		std::atomic<uint64_t> m_diagnosticLastCallbackSamples{0};
		std::atomic<uint64_t> m_diagnosticMinimumCallbackSamples{~uint64_t{0}};
		std::atomic<uint64_t> m_diagnosticMaximumCallbackSamples{0};
		std::atomic<uint64_t> m_diagnosticPlayingCallbackCount{0};
		std::atomic<uint64_t> m_diagnosticCallbackNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticMaximumCallbackNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticLockWaitNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticMaximumLockWaitNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticResamplerNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticDeviceNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticDeadlineMissCount{0};
		std::atomic<uint64_t> m_diagnosticInvalidDeviceCallbackCount{0};
		std::atomic<uint64_t> m_diagnosticHostCallbackCount{0};
		std::atomic<uint64_t> m_diagnosticHostCallbackNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticMaximumHostCallbackNanoseconds{0};
		std::atomic<uint64_t> m_diagnosticHostDeadlineMissCount{0};
	};
}
