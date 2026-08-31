#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mddsp.h"
#include "mdfrontpanel.h"
#include "mdhostaudioqueue.h"
#include "mdmc.h"
#include "mdpanel.h"
#include "mdrealtimemidiqueue.h"
#include "mdrom.h"
#include "mdstate.h"
#include "mdsysextransfer.h"
#include "mdturbomidi.h"
#include "mdtypes.h"

#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"

namespace md
{
	struct ProfileWorkloadAccess;
	// Convert one stereo codec-ADC sample with the bounds/null behavior used by
	// ESSI1. Free-standing so the host-to-codec mapping can be tested without a ROM.
	dsp56k::TWord hostAudioInputSample(const synthLib::TAudioInputs& _inputs,
		uint32_t _frames, uint32_t _cursor, size_t _channel);

	inline const char* midiTurboSpeedLabel(const uint8_t _code)
	{
		switch(_code)
		{
		case 2: return "2";
		case 3: return "3.33";
		case 4: return "4";
		case 5: return "5";
		case 6: return "6.66";
		case 7: return "8";
		case 8: return "10";
		default: return "1";
		}
	}

	// md::Hardware models the Elektron ColdFire MCU and two DSP56303s. A single
	// deterministic interleave scheduler advances all three processors against a
	// shared machine clock on the calling thread.
	//
	// Topology: DSP2 (voice PRODUCER, 0x600000) -> DSP1 (MIXER/codec, 0x500000) -> DAC.
	// The mixer is the "main" DSP: it drives the audio output and its ESSI frame
	// counter is the master clock the MCU is paced against.
	// -------------------------------------------------------------------------
	class Hardware
	{
	public:
		using AudioOutputs = std::array<std::vector<dsp56k::TWord>, 6>;
		Hardware(const std::vector<uint8_t>& _romData = {}, const std::string& _romName = {},
			MachineModel _model = MachineModel::Machinedrum,
			const std::vector<uint8_t>& _initialPatchRam = {},
			std::shared_ptr<FrontPanelPublisher> _frontPanelPublisher = {},
			std::shared_ptr<MidiSysexTransferProgressPublisher> _midiSysexProgressPublisher = {},
			const std::vector<uint8_t>& _initialUserFlash = {},
			const std::vector<uint8_t>& _factoryFlashCache = {},
			const FlashSectorOverlay& _pendingFlashOverlay = {});
		~Hardware();

		bool isValid() const;
		MachineModel getModel() const { return m_model; }
		bool isMonomachine() const { return m_model == MachineModel::Monomachine; }
		bool isAudioReady() const
		{
			return m_dspMixer.booted() && m_dspProducer.booted();
		}
		uint64_t firmwareFingerprint() const { return m_firmwareFingerprint; }
		uint64_t firmwareUpdateMainFingerprint() const
		{
			return m_firmwareUpdateMainFingerprint;
		}
		size_t firmwareUpdateMainSize() const { return m_firmwareUpdateMainSize; }
		uint64_t hostAudioOverflowCount() const
		{
			return m_schedHostAudioOverflow.load(std::memory_order_relaxed);
		}
		uint64_t midiTurboOverflowCount() const
		{
			return m_midiSysexTransfer.overflowCount();
		}
		size_t queuedMidiRxBytes() const { return m_uc.queuedMidiRxBytes(); }
		size_t midiRxOverflowCount() const { return m_uc.midiRxOverflowCount(); }
		uint64_t midiRxConsumedCount() const { return m_uc.midiRxConsumedCount(); }

		Microcontroller& getUC() { return m_uc; }
		std::vector<uint8_t> copyPatchRam() const;
		std::vector<uint8_t> copyUserFlash() const { return m_uc.copyUserFlash(); }
		std::vector<uint8_t> copyFlashData() const { return m_uc.copyFlashData(); }
		const std::vector<uint8_t>& flashBaseline() const { return m_rom.data(); }
		bool flashDirty() const { return m_uc.flashDirty(); }
		bool factoryFlashCacheReady();
		bool copyFactoryFlashBaseline(std::vector<uint8_t>& _baseline);
		std::vector<uint8_t> copyFactoryFlashCache();
		bool copyPendingFlashOverlay(FlashSectorOverlay& _overlay) const;
		void disqualifyFactoryFlashCache();
		bool replaceFactoryFlashCache(const std::vector<uint8_t>& _cache);
		bool replaceFlashData(const std::vector<uint8_t>& _data, const bool _dirty)
		{
			if(_dirty)
				registerExternalInteraction();
			return m_uc.replaceFlashData(_data, _dirty);
		}

		// Role accessors used by the HI08 bridge and scheduler.
		Dsp& getDspProducer() { return m_dspProducer; }	// DSP2, index 1
		Dsp& getDspMixer()    { return m_dspMixer; }	// DSP1, index 0 (main/output)

		void processUC();
		void processAudio(uint32_t _frames, uint32_t _latency);
		void processAudio(const synthLib::TAudioOutputs& _outputs, uint32_t _frames, uint32_t _latency);
		void processAudio(const synthLib::TAudioInputs& _inputs,
			const synthLib::TAudioOutputs& _outputs, uint32_t _frames, uint32_t _latency);

		// Advance the whole machine by _machineFrames codec frames of shared
		// machine time on the calling thread, with NO background threads. One frame = g_dsp1CyclesPer
		// EsaiFrame (2304) DSP cycles = g_ucClockHz/g_samplerate (577.03) UC cycles. UC + both DSPs are
		// stepped event-driven (advance the most-lagging against the shared clock; synchronous HI08
		// catch-up at every host access). Drains the codec output ring so the mixer never blocks.
		// processAudio retains the drained frames; headless callers discard them.
		void advance(uint32_t _machineFrames);

		// Advance DSP _dspIndex to the UC's current machine-time position
		// before a host (HI08) access - MAME's catch_up_elapsed_time. Called from the DSP-side HI08
		// bridge so that every ColdFire read/write/CVR sees the target DSP at the same machine time,
		// which is what makes the boot handshake (UC poll <-> DSP reply) converge deterministically.
		void schedCatchUpDsp(uint32_t _dspIndex);
		void notifyHostPumpStateChanged();

		// Mark the start of a Machinedrum DMA receive window. No-op for MM.
		void mdLinkWindowFlushed();

		bool sendMidi(const synthLib::SMidiEvent& _ev);
		// Audio-thread-only producer path for small, already-encoded semantic
		// commands. Unlike sendMidi(), this never allocates or waits for space.
		bool trySendRealtimeMidi(const uint8_t* _bytes, size_t _count)
		{
			m_externalInteraction.store(true, std::memory_order_relaxed);
			return m_realtimeMidiIn.tryPush(_bytes, _count);
		}
		template<size_t Count>
		bool trySendRealtimeMidi(const std::array<uint8_t, Count>& _bytes)
		{
			return m_realtimeMidiIn.tryPush(_bytes);
		}
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
		{
			m_uc.readMidiOut(_midiOut);
		}
		// Queue a file-sized SysEx stream for paced delivery to the MIDI input.
		// The instrument must already be in its normal SysEx receive mode.
		// Commit a previously validated, caller-owned stream without allocating or
		// copying under the transfer lock. On busy rejection, _transfer is unchanged.
		bool startMidiSysexTransfer(PreparedMidiSysexTransfer& _transfer);
		MidiSysexTransferProgress getMidiSysexTransferProgress() const;

		// Queue a front-panel button/encoder event ([row][mask]). trySendPanelEvent is bounded,
		// thread-safe, allocation-free, and never waits; false means the complete
		// packet was rejected and must be retried if it cannot be lost. The void
		// wrapper retains legacy best-effort behavior, with drops visible via telemetry.
		bool trySendPanelEvent(uint8_t _cmd, uint8_t _arg);
		void sendPanelEvent(uint8_t _cmd, uint8_t _arg)
		{
			(void)trySendPanelEvent(_cmd, _arg);
		}
		size_t getPendingPanelInputBytes() const;
		size_t getPanelInputOverflowCount() const;

		const auto& getAudioOutputs() const { return m_audioOutputs; }
		const std::string& getRomFilename() const { return m_rom.getFilename(); }

		// Last complete front-panel value published by the emulation thread. Returning
		// by value prevents consumers from retaining a reference to live decoder state.
		FrontPanel getFrontPanelSnapshot() const { return m_frontPanelPublisher->read(); }
		bool tryGetFrontPanelSnapshot(FrontPanel& _panel) const
		{
			return m_frontPanelPublisher->tryRead(_panel);
		}

	private:
		friend struct ProfileWorkloadAccess;
		Hardware(bool _syntheticProfile, const std::vector<uint8_t>& _romData,
			const std::string& _romName, MachineModel _model,
			const std::vector<uint8_t>& _initialPatchRam,
			std::shared_ptr<FrontPanelPublisher> _frontPanelPublisher,
			std::shared_ptr<MidiSysexTransferProgressPublisher> _midiSysexProgressPublisher,
			const std::vector<uint8_t>& _initialUserFlash,
			const std::vector<uint8_t>& _factoryFlashCache,
			const FlashSectorOverlay& _pendingFlashOverlay);
		void ensureBufferSize(uint32_t _frames);
		bool finalizeFactoryFlashBaselineLocked();
		void registerExternalInteraction();
		void pumpDsp2HostRequest();		// DSP2 HI08 HREQ -> ColdFire external IRQ4 (see .cpp)
		void onEssiCallbackMixer();		// master clock: advance the ESSI frame counter
		template<typename InactiveObserved>
		void pumpMidiIngressImpl(InactiveObserved&& _inactiveObserved);
		void pumpMidiIngress();

		const MachineModel m_model;
		Rom m_rom;
		const uint64_t m_firmwareFingerprint;
		uint64_t m_firmwareUpdateMainFingerprint = 0;
		size_t m_firmwareUpdateMainSize = 0;
		Microcontroller m_uc;
		std::atomic<bool> m_externalInteraction{false};
		std::atomic<bool> m_factoryFlashReady{false};
		mutable std::mutex m_factoryFlashMutex;
		std::vector<uint8_t> m_factoryFlashCache;
		std::vector<uint8_t> m_factoryFlashBaseline;
		FlashSectorOverlay m_pendingFlashOverlay;
		std::vector<uint8_t> m_pendingPatchRam;
		std::atomic<bool> m_pendingFlashRestoreFailed{false};
		FrontPanel m_frontPanel;	// writer-owned UART2 LCD/LED decoder
		std::shared_ptr<FrontPanelPublisher> m_frontPanelPublisher;
		TurboMidiTransfer m_midiSysexTransfer;
		Dsp m_dspMixer;		// index 0 = DSP1 (0x500000), receives the ring, drives the DAC
		Dsp m_dspProducer;	// index 1 = DSP2 (0x600000), produces voices into the ring

		AudioOutputs m_audioOutputs;
		// Per-machine age of the last shallow link ring. This participates in the
		// MM stall-purge decision, so it must never be shared by concurrently
		// running Hardware instances (as it was when this lived as a static local).
		std::array<uint64_t, 2> m_linkLastShallow{};

		// Codec frames produced by the mixer.
		uint32_t m_esaiFrameIndex = 0;			// codec frames produced

		// ---------------------------------------------------------------------------------------
		// Deterministic interleave scheduler state. advance() maintains a shared machine clock in codec
		// frames and steps UC + both DSPs event-driven against it. Each DSP's cycle counter is
		// rate-locked to the clock from the frame it becomes runnable (origin latched here). See
		// advance() in mdhardware.cpp for the loop and timing constants.
		// ---------------------------------------------------------------------------------------
		bool     schedStep();					// one advance() event-loop iteration; false once all caught up
		double   schedDspFramePos(uint32_t _dspIndex);	// a runnable DSP's machine-frame position
		void     schedDrainCodecOutput();		// pop the mixer ESSI1 output ring so its TX never blocks
		void     schedCatchUpDspToDsp(uint32_t _consumer, uint32_t _producer);
		// Compact, preallocated host-facing storage keeps codec draining bounded.
		// Overflow retains the newest frames and is explicit telemetry; processAudio
		// drains the queue every callback so stale audio cannot accumulate between blocks.
		RealtimeHostAudioQueue m_schedHostAudio;
		std::atomic<uint64_t> m_schedHostAudioOverflow{0};
		bool     m_schedHostAudioActive = false;	// retain drained frames for a host callback
		synthLib::TAudioInputs m_hostAudioInputs{};
		uint32_t m_hostAudioInputFrames = 0;
		uint32_t m_hostAudioInputCursor = 0;
		bool m_hostAudioInputActive = false;
		bool     m_schedInLinkDelivery = false;	// reentrancy guard for cross-DSP catch-up
		bool     m_schedBoundedJit = false;		// experimental cycle-bounded background slices
		double   m_schedFramesTotal   = 0.0;	// machine-time target, accumulated codec frames
		uint64_t m_schedUcCyclesDone  = 0;		// UC cycles executed under the scheduler (processUC)
		uint64_t m_mmBpSinceUcCycles[2] = {0,0};// MM backpressure: UC cycle+1 when a DSP's stall began (0 = none)
		bool     m_mdLinkRoeEngaged = false;	// latched at the first DMA4 receive window
		bool     m_mdLinkAwaitFresh = false;	// waits for DSP2's first word in a receive window
		bool     m_mdOnDemandRendezvousArmPending = false;
		bool     m_mdOnDemandRendezvousActive = false;
		bool     m_mdProducerPortCPending = false;
		dsp56k::TWord m_mdProducerPortCVisible = 0;
		dsp56k::TWord m_mdProducerPortCPendingLevel = 0;
		uint64_t m_mdLinkFlushEpoch = 0;
		uint64_t m_mdProducerPortCPendingEpoch = 0;
		uint64_t m_mdProducerPortCReleaseEpoch = 0;
		std::atomic<bool> m_mmLinkAwaitFresh{false};	// PDRC edge awaits DSP2's DMA reply
		std::atomic<uint64_t> m_mmLinkStrobeEpoch{0};	// cancels delivery after nested catch-up
		uint32_t m_mmLinkStrobeLevel = 2;		// mixer-context edge detector; 2 = no level observed yet
		bool     m_schedDspOriginLatched[2] = { false, false };	// [0]=mixer/DSP1, [1]=producer/DSP2
		double   m_schedDspOriginFrame [2]  = { 0.0, 0.0 };		// machine-frame at runnable transition
		uint64_t m_schedDspOriginCycles[2]  = { 0, 0 };			// getCycles() at that transition
		std::atomic<bool> m_schedulerHostPumpDirty{true};
		// MIDI
		dsp56k::RingBuffer<synthLib::SMidiEvent, 16384, true> m_midiIn;
		size_t m_midiInByteCursor = 0;
		RealtimeMidiByteQueue<64> m_realtimeMidiIn;

		// Panel input events pending delivery to UART2 RX.
		PanelInputQueue m_panelIn;

	};
}
