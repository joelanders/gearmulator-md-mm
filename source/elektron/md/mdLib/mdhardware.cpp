#include "mdhardware.h"

#include "mdfirmwareupdate.h"
#include "mdmemorymap.h"
#include "mdmmwaveforms.h"
#include "mdsysexautomation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#include "mdromloader.h"
#include "mdtypes.h"

#include "dsp56kEmu/jitblockinfo.h"

namespace md
{
	// ColdFire MCF5206E system clock. The MAME driver clocks the CPU from a 25.447 MHz
	// crystal (elektronmono.cpp); used to convert mixer-DSP execution -> UC cycle budget. The
	// MD Sim doesn't model a PLL yet, so this is the fixed nominal rate.
	constexpr uint64_t g_ucClockHz = 40'000'000;

	// One codec (ESSI1) stereo frame corresponds to a fixed number of DSP1-executed cycles. The
	// firmware configures a 96-cycle base link slot; the ESSI1 divider and two stereo slots produce
	// 2304 cycles per codec frame at the 101.6064 MHz DSP clock.
	// The UC is granted g_ucClockHz/44100 = 577 cycles per such frame, matching the
	// hardware's 25.447/101.6064 MHz clock ratio.
	constexpr uint64_t g_dsp1CyclesPerEsaiFrame  = 2304;

	Rom initRom(const std::vector<uint8_t>& _romData, const std::string& _romName,
		const MachineModel _model, const bool _syntheticProfile)
	{
		if(_syntheticProfile)
			return Rom(_romData, _romName);
		if(_romData.empty())
			return RomLoader::findROM(_model);
		Rom rom(_romData, _romName);
		if(rom.isValid() && RomLoader::isRomForModel(rom.data(), _model))
			return rom;
		return RomLoader::findROM(_model);
	}

	std::vector<uint8_t> initMainRam(const Rom& _rom)
	{
		std::vector<uint8_t> result;
		if(firmwareUpdate::isConvertedRom(_rom.data()))
			firmwareUpdate::readSection(_rom.data(), firmwareUpdate::Section::MainOs, result);
		return result;
	}

	std::vector<uint8_t> initPatchRam(const Rom& _rom,
		const std::vector<uint8_t>& _requested)
	{
		if(_requested.size() == memorymap::g_patchBootstrap.size())
			return _requested;
		std::vector<uint8_t> factory;
		if(!firmwareUpdate::isConvertedRom(_rom.data())
			|| !firmwareUpdate::readSection(_rom.data(),
				firmwareUpdate::Section::FactoryData, factory)
			|| factory.size() > memorymap::g_patchBootstrap.size())
			return _requested;
		factory.resize(memorymap::g_patchBootstrap.size(), 0);
		// The physical bootstrap writes this warm-start cookie after expanding the
		// factory image. Recreate it because direct update boot bypasses that code.
		constexpr std::array<uint8_t, 4> cookie{{'D','A','V','E'}};
		std::copy(cookie.begin(), cookie.end(), factory.end() - 8);
		return factory;
	}

	uint64_t fingerprintRom(const std::vector<uint8_t>& _data)
	{
		uint64_t result = 14695981039346656037ull;
		for(const auto byte : _data)
		{
			result ^= byte;
			result *= 1099511628211ull;
		}
		return result;
	}

	dsp56k::TWord hostAudioInputSample(const synthLib::TAudioInputs& _inputs,
		const uint32_t _frames, const uint32_t _cursor, const size_t _channel)
	{
		if(_channel >= 2 || _cursor >= _frames || !_inputs[_channel])
			return 0;
		return dsp56k::sample2dsp(_inputs[_channel][_cursor]);
	}

	Hardware::Hardware(const std::vector<uint8_t>& _romData, const std::string& _romName,
		const MachineModel _model, const std::vector<uint8_t>& _initialPatchRam,
		std::shared_ptr<FrontPanelPublisher> _frontPanelPublisher,
		std::shared_ptr<MidiSysexTransferProgressPublisher> _midiSysexProgressPublisher,
		const std::vector<uint8_t>& _initialUserFlash,
		const std::vector<uint8_t>& _factoryFlashCache,
		const FlashSectorOverlay& _pendingFlashOverlay)
		: Hardware(false, _romData, _romName, _model, _initialPatchRam,
			std::move(_frontPanelPublisher), std::move(_midiSysexProgressPublisher),
			_initialUserFlash, _factoryFlashCache, _pendingFlashOverlay)
	{
	}

	Hardware::Hardware(const bool _syntheticProfile,
		const std::vector<uint8_t>& _romData, const std::string& _romName,
		const MachineModel _model, const std::vector<uint8_t>& _initialPatchRam,
		std::shared_ptr<FrontPanelPublisher> _frontPanelPublisher,
		std::shared_ptr<MidiSysexTransferProgressPublisher> _midiSysexProgressPublisher,
		const std::vector<uint8_t>& _initialUserFlash,
		const std::vector<uint8_t>& _factoryFlashCache,
		const FlashSectorOverlay& _pendingFlashOverlay)
		: m_model(_model)
		, m_rom(initRom(_romData, _romName, _model, _syntheticProfile))
		, m_firmwareFingerprint(_syntheticProfile ? 0 : fingerprintRom(m_rom.data()))
		, m_uc(m_rom, m_model, initPatchRam(m_rom,
			_pendingFlashOverlay.valid ? std::vector<uint8_t>{} : _initialPatchRam),
			initMainRam(m_rom), _initialUserFlash)
		// A complete project image can boot directly without a local factory cache,
		// but it must never become the machine-local factory baseline itself.
		, m_externalInteraction(_model == MachineModel::Machinedrum
			&& !_initialUserFlash.empty() && _factoryFlashCache.empty()
			&& !_pendingFlashOverlay.valid)
		, m_factoryFlashReady(!_factoryFlashCache.empty())
		, m_factoryFlashCache(_factoryFlashCache)
		, m_factoryFlashBaseline(_model == MachineModel::Machinedrum
			&& _factoryFlashCache.empty() ? g_romSize : 0)
		, m_pendingFlashImage(_pendingFlashOverlay.valid ? g_romSize : 0)
		, m_pendingFlashOverlay(_pendingFlashOverlay)
		, m_pendingPatchRam(_pendingFlashOverlay.valid
			? _initialPatchRam : std::vector<uint8_t>{})
		, m_pendingFlashRestoreActive(_pendingFlashOverlay.valid)
		, m_frontPanelPublisher(_frontPanelPublisher
			? std::move(_frontPanelPublisher)
			: std::make_shared<FrontPanelPublisher>())
		, m_midiSysexTransfer(g_ucClockHz, std::move(_midiSysexProgressPublisher))
		, m_dspMixer(*this, m_uc.getHdi08Dsp1(), 0)		// DSP1, mixer/main
		, m_dspProducer(*this, m_uc.getHdi08Dsp2(), 1)	// DSP2, producer
	{
		// Experimental, default-off performance gate. A single binary can run the
		// reference dispatcher and the cycle-bounded trampoline for exact A/B tests.
		m_schedBoundedJit = std::getenv("GEARMULATOR_MDMM_BOUNDED_JIT") != nullptr;

		if(!m_rom.isValid())
			return;
		std::vector<uint8_t> updateMainOs;
		if(!_syntheticProfile && firmwareUpdate::readSection(m_rom.data(),
			firmwareUpdate::Section::MainOs, updateMainOs))
		{
			m_firmwareUpdateMainSize = updateMainOs.size();
			m_firmwareUpdateMainFingerprint = fingerprintRom(updateMainOs);
		}
		if(!_syntheticProfile && isMonomachine())
		{
			auto& mixerMemory = m_dspMixer.dsp().memory();
			auto& producerMemory = m_dspProducer.dsp().memory();
			// External X, Y, and P share one backing store above the DSP bridge
			// address, so one X write initializes the bank seen through either
			// data-memory area on each DSP.
			const bool loaded = mmwaveforms::loadFactoryBank(m_rom.data(),
				[&mixerMemory, &producerMemory](const uint32_t _address, const uint32_t _value)
				{
					mixerMemory.set(dsp56k::MemArea_X, _address, _value);
					producerMemory.set(dsp56k::MemArea_X, _address, _value);
				});
			if(!loaded)
				std::fprintf(stderr, "[MM] ROM has no valid MKII factory DigiPRO waveform bank: %s\n",
					m_rom.getFilename().c_str());
		}
		m_mdOnDemandRendezvousArmPending = !_syntheticProfile && !isMonomachine()
			&& m_firmwareFingerprint == g_mdOs163Fingerprint;

		// Observe transmitted bytes for request/response protocols without
		// consuming the plug-in's normal MIDI output.
		m_uc.setMidiTransmitTap([this](const uint8_t _byte)
		{
			m_midiSysexTransfer.observeTransmitByte(_byte);
		});

		if(isMonomachine())
		{
			const auto wake = [this] { notifyHostPumpStateChanged(); };
			m_dspMixer.setHostPumpWakeCallback(wake);
			m_dspProducer.setHostPumpWakeCallback(wake);
		}

		// Feed the OS's host->panel UART2 stream into the front-panel LCD/LED decoder.
		m_uc.setFrontPanel(&m_frontPanel);
		const auto panelPublisher = m_frontPanelPublisher;
		m_uc.setPanelLedTransitionCallback(
			[panelPublisher](const uint8_t _command, const uint8_t _value,
				const uint64_t _emulationCycles)
			{
				(void)panelPublisher->tryPushLedTransition(
					_command, _value, _emulationCycles);
			});

		// Inter-DSP ESSI0 ring. Each DSP's
		// ESSI0 TX pushes its frame into the OTHER DSP's ESSI0 audio-INPUT ring (blocking
		// push_back on the deep Lock=true, 32768-frame ring); each DSP's ESSI0 RX blocks popping
		// its OWN input ring. So a consumer NEVER reads fabricated silence - it waits for the real
		// frame, keeping consumption equal to production. The input rings are prefilled
		// (writeEmptyAudioIn(64) in the Dsp constructor) so
		// neither side of the FULL-DUPLEX link blocks at startup, and execTX runs before execRX
		// each slot (esaiclock), so each DSP feeds its neighbour before it can block on its own
		// RX - no deadlock, provided the two ESSI0 clock rates match (they do: same divider config
		// on both DSPs). Codec ESSI1 RX is callback-fed and remains non-blocking: MD receives
		// the current host input block, while MM and out-of-block reads receive silence.
		const auto txToRx = [](const dsp56k::Audio::TxFrame& _tx, dsp56k::Audio::RxFrame& _rx)
		{
			_rx.resize(_tx.size());
			for(size_t i = 0; i < _tx.size(); ++i)
				_rx[i] = dsp56k::Audio::RxSlot{_tx[i][0]};	// the MD link carries one word per slot
		};

		// Both DSPs run on one scheduler thread, so a blocking ring push/pop
		// (which parks the calling thread until the peer thread drains/fills) would deadlock. Under
		// the scheduler the inter-DSP ESSI0 ring becomes non-blocking: drop-on-full for the producer
		// push, silence-on-empty for the consumer pop. Ordering/level correctness comes from the
		// scheduler advancing the peer before delivery and, when it lands, the hardware-true
		// skip-on-empty link RX.
		const auto pushToInput = [txToRx, this](dsp56k::Essi& _consumer, const uint32_t _selfDsp)
		{
			return [txToRx, this, &_consumer, _selfDsp](uint64_t& _frameIndex, const dsp56k::Audio::TxFrame& _values)
			{
				dsp56k::Audio::RxFrame rx;
				txToRx(_values, rx);
				auto& ring = _consumer.getAudioInputs();
				const bool mdProducerToMixer = _selfDsp == 1 && !isMonomachine();
				const bool rendezvousActiveBefore = mdProducerToMixer
					&& m_mdOnDemandRendezvousActive;
				const uint64_t flushEpochBefore = mdProducerToMixer
					? m_mdLinkFlushEpoch : 0;
					// The legacy delivery path advances the consumer (the DSP whose input ring this
					// is, index 1-_selfDsp) to the producer's current machine time BEFORE enqueueing, so
					// the frame lands at the right point in the consumer's timeline (edge-preserving,
					// no stale/early consumption). Then enqueue non-blocking (drop-on-full; the ring
					// stays shallow because the consumer was just caught up). Gated to the post-boot
					// audio phase so it can never perturb the loader handshake (see the floor const).
					// The on-demand path below deliberately pre-enqueues its wire edge.
					// A serial wire has no memory: a word
					// clocked out while the consumer's receiver is disabled is gone on real hardware.
					// Enqueueing those words instead lets the boot-era stream (producer clocking,
					// consumer still in its loader with ESSI0 RE clear) peg this ring at capacity
					// and replay stale data after the receiver starts. A serial wire has no
					// such backlog, so discard data while receive is disabled.
					if(!_consumer.hasEnabledReceivers())
					{
						++_frameIndex;
						return;
					}
					// The MD link is a synchronous on-demand wire. Once the
					// request edge has been released for this DMA4 window, put
					// each fresh word on the receiver wire before advancing DSP1 to the
					// producer's timestamp. Rejected/stale callbacks still advance time but
					// never become a later wire edge.
					if(rendezvousActiveBefore)
					{
						auto& dma4 = m_dspMixer.getPeriph().getDMA();
						const bool releasedForWindow = !m_mdProducerPortCPending
							&& m_mdProducerPortCReleaseEpoch == flushEpochBefore;
						const bool dma4Active = (dma4.getDCR(4)
							& (1u << dsp56k::DmaChannel::De)) != 0;
						const bool fresh = m_dspProducer.getPeriph().getEssi0()
							.getLastTxWrittenMask() != 0;
						if(fresh && releasedForWindow && dma4Active && !ring.full())
							ring.push_back(std::move(rx));
						schedCatchUpDspToDsp(1u - _selfDsp, _selfDsp);
						++_frameIndex;
						return;
					}

					// Typed early-link-catch-up construction selects floor 0; otherwise the post-boot
					// gate is 256. Floor 0 runs MAME-style consumer catch-up during the boot window.
					const uint64_t strobeEpoch = (isMonomachine() && _selfDsp == 1)
						? m_mmLinkStrobeEpoch.load(std::memory_order_acquire) : 0;
					// With an explicit floor, fire when esaiFrameIndex >= floor (floor=0 => ALWAYS,
					// including the boot window). Default keeps the strict post-boot gate (> 256).
					// Rendezvous (catch-up-before-delivery): advance the CONSUMER toward the producer's
					// shared-clock time before delivery. Left under the existing floor control - per-word
					// when the floor is 0, per-interaction when raised. The 2048-deep transport
					// below absorbs a slice's burst, so per-word catch-up is NOT required for throughput;
					// the floor controls transport fidelity versus rendezvous tightness.
					schedCatchUpDspToDsp(1u - _selfDsp, _selfDsp);

					// Consumer catch-up can open a new receive window inside this
					// already-snapshotted callback. The current word
					// belongs to the completed interval and must not enter the new window.
					if(mdProducerToMixer && !rendezvousActiveBefore
						&& m_mdOnDemandRendezvousActive)
					{
						++_frameIndex;
						return;
					}

					// The catch-up above executes DSP1 inline. If it crossed a new PDRC
					// request, this callback's word belongs to the completed interval and
					// must not be enqueued after the request cleared the receive history.
					if(isMonomachine() && _selfDsp == 1 &&
						m_mmLinkStrobeEpoch.load(std::memory_order_acquire) != strobeEpoch)
					{
						++_frameIndex;
						return;
					}

					// Model continuous receiver-overrun semantics on the MD
					// DSP1 link RX. Silicon holds at most ONE uncollected word (the RX register, RDF set)
					// plus the in-flight shift word; a word arriving while RDF is still set fails the
					// shift->RX transfer and is DESTROYED at arrival - the OLD word is kept, ROE is set
					// (DSP56303UM Table 7-5). Without this the ring retains a standing 1-2 word residue of
					// DSP2's idle-slot retransmits across the DMA4 window boundary. In a
					// window RDF is set and collected within the same RX tick (triggerByRequest transfers
					// synchronously), so RDF observed set here means genuinely uncollected: DMA4 unarmed.
					// Engage only after DMA4 opens the steady-state receive window so
					// bootstrap traffic retains catch-up delivery.
					// MD only: the MM's flow-controlled burst link legitimately queues between strobes.
					if(_selfDsp == 1 && !isMonomachine())
					{
						if(!m_mdLinkRoeEngaged && _consumer.isFastLinkRx() &&
							(m_dspMixer.getPeriph().getDMA().getDCR(4) & (1u << dsp56k::DmaChannel::De)))
						{
							m_mdLinkRoeEngaged = true;
						}
						if(m_mdLinkRoeEngaged && _consumer.getSR().test(dsp56k::Essi::SSISR_RDF))
						{
							_consumer.setReceiverOverrun();
							++_frameIndex;
							return;
						}
						// Between a receive-window flush and DSP2's first DMA-fed TX slot,
						// the scheduler may create idle retransmits of DSP2's retained register.
						// On the shared wire clock those words complete before the flush and die with
						// it; the emulated per-DSP slot grids land them as the window's head cells
						// instead. Apply the flush semantics using the ESSI's per-slot underrun status, never
						// by count; the receiver-overrun handling above already disposes the in-flight word
						// in some windows.
						// The first DMA-fed word disarms this path. This is flush disposal,
						// not an overrun.
						if(m_mdLinkAwaitFresh)
						{
						if(m_dspProducer.getPeriph().getEssi0().getLastTxWrittenMask() == 0)
						{
							++_frameIndex;
								return;
							}
							m_mdLinkAwaitFresh = false;
						}
					}
					if(!ring.full())
						ring.push_back(std::move(rx));
				++_frameIndex;
			};
		};

		const auto blockingPop = [this](dsp56k::Essi& _self, const uint32_t _selfDsp)
		{
			return [this, &_self, _selfDsp](uint64_t& _frameIndex, dsp56k::Audio::RxFrame& _frame)
			{
				auto& ring = _self.getAudioInputs();
					// Stall recovery: under scheduler link catch-up the consumer is
					// advanced to the producer's time before every enqueue, so this ring can only be
					// DEEP if the consumer's RX stopped clocking for a while as the wire kept running
					// (constructor prefill, receive-enable transient, or ESSI reconfiguration).
					// Real hardware loses stalled words by receiver overrun and
					// resumes at the CURRENT stream position; a deep ring instead replays the stall
					// backlog forever. Purge only well beyond normal lockstep variation.
					// A raw depth>16 check cannot tell the two apart, because the MM's flow-controlled
					// link legitimately bursts. The distinguishing behavior is that a stall residue never drains
					// (the offset is permanent), while a burst drains to empty before the next strobe.
					// So purge only when the ring's MINIMUM depth over a window of pops stays deep.
					{
						// The MD streams in word lockstep, while the MM uses flow-controlled
						// bursts. For MM,
						// purge only rings that have not been shallow for >1024 codec frames (a burst
						// is shallow again within ~1 block period; a genuine backlog is not). The
						// active MD rendezvous instead carries future edges directly and bypasses
						// this legacy purge; strict skip-on-empty RX supplies the hardware boundary.
						const bool s_immediate = !isMonomachine();
						const bool preserveRendezvousFutureEdges =
							m_mdOnDemandRendezvousActive && _selfDsp == 0
							&& !isMonomachine();
						const uint64_t esaiNow = m_esaiFrameIndex;
						auto& lastShallow = m_linkLastShallow[_selfDsp];
						if(ring.size() <= 16)
							lastShallow = esaiNow;
						else if(!preserveRendezvousFutureEdges
							&& (s_immediate || esaiNow - lastShallow > 1024))
						{
							while(!ring.empty())
								ring.pop_front();
							lastShallow = esaiNow;	// ring is now empty (shallow)
						}
					}
					// Non-blocking on the single scheduler thread: silence on empty rather than park.
					// Hardware-true skip-on-empty link receive replaces this with matched consumption
					// and production once the frame has landed.
					if(ring.empty())
						_frame.clear();
					else
						_frame = ring.pop_front();
				++_frameIndex;
			};
		};

		const auto silence = [](uint64_t& _frameIndex, dsp56k::Audio::RxFrame& _frame)
		{
			_frame.clear();
			++_frameIndex;
		};
		const auto codecInput = [this](uint64_t& _frameIndex,
			dsp56k::Audio::RxFrame& _frame)
		{
			const auto cursor = m_hostAudioInputCursor++;
			const auto sample = [this, cursor](const size_t _channel)
			{
				return m_hostAudioInputActive
					? hostAudioInputSample(m_hostAudioInputs,
						m_hostAudioInputFrames, cursor, _channel)
					: dsp56k::TWord{0};
			};
			_frame.resize(2);
			_frame[0] = dsp56k::Audio::RxSlot{sample(0)};
			_frame[1] = dsp56k::Audio::RxSlot{sample(1)};
			++_frameIndex;
		};

		// ESSI0 inter-DSP ring, full-duplex: DSP2 TX -> DSP1 input and vice versa.
		{
			auto fwd = pushToInput(m_dspMixer.getPeriph().getEssi0(), 1);
			m_dspProducer.getPeriph().getEssi0().setWriteTxCallback(
				[this, fwd = std::move(fwd)](uint64_t& _frameIndex, const dsp56k::Audio::TxFrame& _values)
				{
					// MM PDRC strobe rendezvous: make the request/response boundary atomic in
					// emulated time. Under the coarse single-thread scheduler DSP2 can already
					// be tens of thousands of cycles ahead when DSP1 raises the request, then
					// emit retained-register underruns before its polling loop observes the new
					// level. Real DSPs observe the edge concurrently. Suppress only that
					// scheduler-created prefix; after the first DMA-fed word, normal ESSI
					// underrun/retransmit semantics apply again.
					if(isMonomachine() && m_mmLinkAwaitFresh.load(std::memory_order_acquire))
					{
						const bool dma4Active =
							(m_dspMixer.getPeriph().getDMA().getDCR(4) &
								(1u << dsp56k::DmaChannel::De)) != 0;
						const bool dma1Active =
							(m_dspProducer.getPeriph().getDMA().getDCR(1) &
								(1u << dsp56k::DmaChannel::De)) != 0;
						const auto writtenMask =
							m_dspProducer.getPeriph().getEssi0().getLastTxWrittenMask();
						if(!dma4Active || !dma1Active || writtenMask == 0)
						{
							++_frameIndex;
							return;
						}
						m_mmLinkAwaitFresh.store(false, std::memory_order_release);
					}

					fwd(_frameIndex, _values);
				});
		}
		m_dspMixer.getPeriph().getEssi0().setWriteTxCallback(pushToInput(
			m_dspProducer.getPeriph().getEssi0(), 0));
		m_dspMixer.getPeriph().getEssi0().setReadRxCallback(blockingPop(
			m_dspMixer.getPeriph().getEssi0(), 0));
		m_dspProducer.getPeriph().getEssi0().setReadRxCallback(blockingPop(
			m_dspProducer.getPeriph().getEssi0(), 1));
		// The Machinedrum mixer's ESSI1 is connected to the stereo codec ADC. The
		// producer has no independent host input path, and MM behavior remains the
		// established silent-input model until its input machines are qualified.
		if(isMonomachine())
			m_dspMixer.getPeriph().getEssi1().setReadRxCallback(silence);
		else
			m_dspMixer.getPeriph().getEssi1().setReadRxCallback(codecInput);
		m_dspProducer.getPeriph().getEssi1().setReadRxCallback(silence);

		// Each mixer ESSI1 output frame advances the codec frame counter used by
		// the audio plumbing.
		m_dspMixer.getPeriph().getEssi1().setCallback([this](dsp56k::Audio*){ onEssiCallbackMixer(); });

		// Inter-DSP clock wiring. Each DSP runs the same program, which probes its ESSI1
		// pins (Port D bits 2/3 = SC12 frame sync / SCK1 bit clock, read as GPIO) to decide
		// its role: quiet pins -> "I am the clock master" (ESSI1 TX on, internal clock),
		// running clock -> slave. On the board the mixer (DSP1) drives the codec clock and
		// the producer (DSP2) receives it, so the mixer's inputs stay quiet (nothing sets a
		// host input source -> reads 0) and the producer sees a running clock. The producer's
		// ESSI0 SC01 pin (Port C bit 1) additionally carries the link frame sync the program
		// paces itself against (MAME models the same signal as a synthesized toggle every
		// 147456 cycles). All modelled as instruction-counter derived square waves, evaluated
		// in the reading DSP's execution context.
		{
			const auto& cnt = m_dspProducer.dsp().getInstructionCounter();

			m_dspProducer.getPeriph().getPortD().setHostInputSource([&cnt]() -> dsp56k::TWord
			{
				const auto c = cnt;
				dsp56k::TWord v = 0;
				if((c >> 4) & 1)		v |= (1<<3);	// SCK1: codec bit clock
				if((c / 522) & 1)		v |= (1<<2);	// SC12: codec frame sync (~1x sample rate)
				return v;
			});

			// Port C bit 1 carries block sync. Delay a transition until the mixer
			// opens its corresponding DMA receive window.
			m_dspMixer.getPeriph().getPortC().setCallbackDspWrite([this]
			{
				const dsp56k::TWord level = m_dspMixer.getPeriph().getPortC().hostRead() & (1u << 1);
				if(!isMonomachine() && m_mdOnDemandRendezvousActive)
				{
					const auto desiredLevel = m_mdProducerPortCPending
						? m_mdProducerPortCPendingLevel : m_mdProducerPortCVisible;
					if(level == desiredLevel)
						return;
					m_mdProducerPortCPending = true;
					m_mdProducerPortCPendingLevel = level;
					m_mdProducerPortCPendingEpoch = m_mdLinkFlushEpoch;
					return;
				}
				if(!isMonomachine())
					m_mdProducerPortCVisible = level;
				const uint32_t strobeLevel = level ? 1u : 0u;
				if(isMonomachine() && strobeLevel != m_mmLinkStrobeLevel)
				{
					m_mmLinkStrobeLevel = strobeLevel;
					const bool dma4Idle =
						(m_dspMixer.getPeriph().getDMA().getDCR(4) &
							(1u << dsp56k::DmaChannel::De)) == 0;
					const bool dma1Idle =
						(m_dspProducer.getPeriph().getDMA().getDCR(1) &
							(1u << dsp56k::DmaChannel::De)) == 0;
					if(dma4Idle && dma1Idle)
					{
						// The RX register is one word deep, not an archival FIFO.
						// Anything queued before this new request belongs to the
						// completed/idle wire interval and cannot precede DSP2's
						// response in the new DMA4 window.
						auto& ring = m_dspMixer.getPeriph().getEssi0().getAudioInputs();
						while(!ring.empty())
							ring.pop_front();
						m_mmLinkAwaitFresh.store(true, std::memory_order_release);
						m_mmLinkStrobeEpoch.fetch_add(1, std::memory_order_acq_rel);
					}
				}
				m_dspProducer.getPeriph().getPortC().hostWrite(level);
			});
		}

		const bool firmwareUpdateBoot = firmwareUpdate::isConvertedRom(m_rom.data());
		if(firmwareUpdateBoot)
		{
			std::vector<uint8_t> mixer;
			std::vector<uint8_t> producer;
			std::vector<uint8_t> common;
			std::vector<uint8_t> mainOs;
			const bool sectionsRead = firmwareUpdate::readSection(m_rom.data(),
				firmwareUpdate::Section::MainOs, mainOs)
				&& firmwareUpdate::readSection(m_rom.data(),
				firmwareUpdate::Section::DspMixer, mixer)
				&& firmwareUpdate::readSection(m_rom.data(),
					firmwareUpdate::Section::DspProducer, producer)
				&& firmwareUpdate::readSection(m_rom.data(),
					firmwareUpdate::Section::DspCommon, common);
			std::string error;
			if(!sectionsRead || !m_dspMixer.loadFirmwareUpdate(mainOs, mixer, common, error)
				|| !m_dspProducer.loadFirmwareUpdate(mainOs, producer, common, error))
			{
				std::fprintf(stderr, "[MD/MM] cannot direct-boot OS update %s: %s\n",
					m_rom.getFilename().c_str(),
					sectionsRead ? error.c_str() : "missing DSP section");
				m_rom.invalidate();
				return;
			}
			// On the instrument the DSPs execute while the much slower ColdFire
			// expands the main OS. Give both programs time to finish their reset-time
			// initialization before starting the already-expanded OS directly.
			const uint64_t updateDspLeadCycles = isMonomachine()
				? 173'400'000u : 335'100'000u;
			while(m_dspMixer.dsp().getCycles() < updateDspLeadCycles
				|| m_dspProducer.dsp().getCycles() < updateDspLeadCycles)
			{
				if(m_dspMixer.dsp().getCycles() <= m_dspProducer.dsp().getCycles())
					m_dspMixer.dsp().exec();
				else
					m_dspProducer.dsp().exec();
			}
			m_uc.getSim().prepareFirmwareUpdateBoot(isMonomachine());
		}

		// Load SP/PC from the reset vectors before scheduled UC execution starts.
		m_uc.reset();
		if(firmwareUpdateBoot)
		{
			uint32_t factoryAddress = 0;
			if(!firmwareUpdate::factoryFlashAddress(m_rom.data(), factoryAddress))
			{
				m_rom.invalidate();
				return;
			}
			m_uc.prepareFirmwareUpdateBoot(factoryAddress);
		}
		m_uc.exec();	// prefetch warm-up (retires nothing; matches the synchronous harness)

	}

	Hardware::~Hardware() = default;

	bool Hardware::isValid() const
	{
		return m_rom.isValid()
			&& !m_pendingFlashRestoreFailed.load(std::memory_order_acquire);
	}

	std::vector<uint8_t> Hardware::copyPatchRam() const
	{
		std::lock_guard lock(m_factoryFlashMutex);
		return m_pendingFlashOverlay.valid ? m_pendingPatchRam : m_uc.copyPatchRam();
	}

	void Hardware::advanceFactoryFlashCapture()
	{
		if(m_model != MachineModel::Machinedrum
			|| m_factoryFlashReady.load(std::memory_order_acquire)
			|| m_pendingFlashRestoreFailed.load(std::memory_order_acquire)
			|| m_externalInteraction.load(std::memory_order_relaxed))
			return;

		constexpr size_t sliceSize = g_uwFlashSectorSize;
		if(!m_factoryFlashCaptureComplete)
		{
			constexpr uint64_t minimumAge = g_ucClockHz * 10;
			constexpr uint64_t quietPeriod = g_ucClockHz * 2;
			if(!m_uc.flashDirty() || m_uc.getCycles() < minimumAge
				|| m_uc.flashIdleCycles() < quietPeriod)
			{
				m_factoryFlashCaptureOffset = 0;
				m_factoryFlashCaptureFingerprint = 14695981039346656037ull;
				return;
			}

			const auto remaining = m_factoryFlashBaseline.size()
				- m_factoryFlashCaptureOffset;
			const auto count = std::min(sliceSize, remaining);
			auto* const destination = m_factoryFlashBaseline.data()
				+ m_factoryFlashCaptureOffset;
			if(!m_uc.copyFlashDataRangeRealtime(destination,
				m_factoryFlashCaptureOffset, count))
				return;
			if(m_pendingFlashOverlay.valid)
				std::copy_n(destination, count, m_pendingFlashImage.begin()
					+ m_factoryFlashCaptureOffset);
			for(size_t i = 0; i < count; ++i)
			{
				m_factoryFlashCaptureFingerprint ^= destination[i];
				m_factoryFlashCaptureFingerprint *= 1099511628211ull;
			}
			m_factoryFlashCaptureOffset += count;
			if(m_factoryFlashCaptureOffset != m_factoryFlashBaseline.size())
				return;
			m_factoryFlashCaptureComplete = true;

			if(m_pendingFlashOverlay.valid
				&& m_pendingFlashOverlay.baselineFingerprint
					!= m_factoryFlashCaptureFingerprint
				&& m_pendingFlashOverlay.baselineFingerprint != fingerprintRom(m_rom.data()))
			{
				m_pendingFlashRestoreFailed.store(true, std::memory_order_release);
				m_pendingFlashRestoreActive.store(false, std::memory_order_release);
				m_externalInteraction.store(true, std::memory_order_relaxed);
				std::fprintf(stderr,
					"[MD] project flash does not match the initialized factory baseline\n");
				return;
			}
		}

		if(!m_pendingFlashOverlay.valid)
		{
			m_factoryFlashReady.store(true, std::memory_order_release);
			return;
		}

		if(m_pendingFlashSectorIndex < m_pendingFlashOverlay.sectors.size())
		{
			const auto index = m_pendingFlashSectorIndex++;
			const auto destination = static_cast<size_t>(
				m_pendingFlashOverlay.sectors[index]) * g_uwFlashSectorSize;
			const auto source = index * static_cast<size_t>(g_uwFlashSectorSize);
			std::copy_n(m_pendingFlashOverlay.data.data() + source,
				g_uwFlashSectorSize, m_pendingFlashImage.begin() + destination);
			return;
		}

		// Host snapshots hold this mutex while selecting pending or published state.
		// Never make the scheduler wait for one; retry at the next callback instead.
		std::unique_lock stateLock(m_factoryFlashMutex, std::try_to_lock);
		if(!stateLock.owns_lock())
			return;
		const auto publishResult = m_uc.publishStateImagesRealtime(
			m_pendingFlashImage, m_pendingPatchRam,
			!m_pendingFlashOverlay.data.empty());
		if(publishResult == Microcontroller::StateImagePublishResult::Busy)
			return;
		if(publishResult != Microcontroller::StateImagePublishResult::Published)
		{
			m_pendingFlashRestoreFailed.store(true, std::memory_order_release);
			m_pendingFlashRestoreActive.store(false, std::memory_order_release);
			m_externalInteraction.store(true, std::memory_order_relaxed);
			return;
		}

		// Retain the backing allocations until Hardware destruction; releasing a
		// multi-megabyte overlay or patch image here would move allocator work back
		// onto the audio callback we just made bounded.
		m_pendingFlashOverlay.valid = false;
		m_pendingFlashRestoreActive.store(false, std::memory_order_release);
		m_externalInteraction.store(true, std::memory_order_relaxed);
		m_factoryFlashReady.store(true, std::memory_order_release);
	}

	bool Hardware::factoryFlashCacheReady()
	{
		return m_factoryFlashReady.load(std::memory_order_acquire);
	}

	bool Hardware::copyFactoryFlashBaseline(std::vector<uint8_t>& _baseline)
	{
		if(!m_factoryFlashReady.load(std::memory_order_acquire))
			return false;
		std::lock_guard lock(m_factoryFlashMutex);
		if(!m_factoryFlashBaseline.empty())
		{
			_baseline = m_factoryFlashBaseline;
			return true;
		}
		return decodeFactoryFlashCache(_baseline, m_factoryFlashCache, m_rom.data());
	}

	std::vector<uint8_t> Hardware::copyFactoryFlashCache()
	{
		if(!m_factoryFlashReady.load(std::memory_order_acquire))
			return {};
		std::lock_guard lock(m_factoryFlashMutex);
		if(m_factoryFlashCache.empty()
			&& !encodeFactoryFlashCache(m_factoryFlashCache,
				m_factoryFlashBaseline, m_rom.data()))
			return {};
		return m_factoryFlashCache;
	}

	bool Hardware::copyPendingFlashOverlay(FlashSectorOverlay& _overlay) const
	{
		std::lock_guard lock(m_factoryFlashMutex);
		if(!m_pendingFlashOverlay.valid)
			return false;
		_overlay = m_pendingFlashOverlay;
		return true;
	}

	bool Hardware::replaceFactoryFlashCache(const std::vector<uint8_t>& _cache)
	{
		std::vector<uint8_t> ignored;
		if(_cache.empty() || !decodeFactoryFlashCache(ignored, _cache, m_rom.data()))
			return false;
		std::lock_guard lock(m_factoryFlashMutex);
		m_factoryFlashCache = _cache;
		m_factoryFlashBaseline.clear();
		m_factoryFlashReady.store(true, std::memory_order_release);
		return true;
	}

	void Hardware::registerExternalInteraction()
	{
		// Pending project data must be installed before external traffic can make
		// the freshly initialized flash authoritative. This path is called from
		// real-time MIDI ingress and therefore remains lock-free and bounded.
		if(!m_pendingFlashRestoreActive.load(std::memory_order_acquire))
			m_externalInteraction.store(true, std::memory_order_relaxed);
	}

	void Hardware::disqualifyFactoryFlashCache()
	{
		registerExternalInteraction();
	}

	void Hardware::mdLinkWindowFlushed()
	{
		if(!m_mdLinkRoeEngaged)
			return;
		++m_mdLinkFlushEpoch;

		if(m_mdOnDemandRendezvousArmPending)
		{
			auto& producerEssi = m_dspProducer.getPeriph().getEssi0();
			auto& mixerEssi = m_dspMixer.getPeriph().getEssi0();
			auto& dma4 = m_dspMixer.getPeriph().getDMA();
			const bool producerOnDemand = producerEssi.getCRB().test(
				dsp56k::Essi::RegCRBbits::CRB_MOD)
				&& producerEssi.getTxWordCount() == 0;
			const bool mixerReady = mixerEssi.isFastLinkRx();
			const bool dma4Idle = (dma4.getDCR(4)
				& (1u << dsp56k::DmaChannel::De)) == 0;
			if(producerOnDemand && mixerReady && dma4Idle)
			{
				m_mdOnDemandRendezvousArmPending = false;
				m_mdOnDemandRendezvousActive = true;
				m_mdProducerPortCPending = false;
				m_mdProducerPortCReleaseEpoch = 0;
				m_dspProducer.getPeriph().getPortC().setHostInputSource([this]()
					-> dsp56k::TWord
				{
					if(m_mdProducerPortCPending)
					{
						auto& activeDma4 = m_dspMixer.getPeriph().getDMA();
						if((activeDma4.getDCR(4)
							& (1u << dsp56k::DmaChannel::De)) != 0)
						{
							m_mdProducerPortCVisible = m_mdProducerPortCPendingLevel;
							m_mdProducerPortCPending = false;
							m_mdProducerPortCReleaseEpoch =
								m_mdProducerPortCPendingEpoch;
						}
					}
					return m_mdProducerPortCVisible;
				});
				producerEssi.setOnDemandTxWireSemantics(true);
				mixerEssi.setOnDemandRxWireSemantics(true);
				m_mdLinkAwaitFresh = false;
			}
		}
		else if(m_mdOnDemandRendezvousActive)
		{
			// A request must have been observed before the next receive window.
			// Discarding an unexpectedly unreleased pin prevents a stale edge from
			// being promoted into the new DMA4 window.
			m_mdProducerPortCPending = false;
		}

		if(m_mdOnDemandRendezvousActive)
			return;
		m_mdLinkAwaitFresh = true;
	}

	bool Hardware::trySendPanelEvent(const uint8_t _cmd, const uint8_t _arg)
	{
		registerExternalInteraction();
		return m_panelIn.tryPush(_cmd, _arg);
	}

	size_t Hardware::getPendingPanelInputBytes() const
	{
		return m_panelIn.size();
	}

	size_t Hardware::getPanelInputOverflowCount() const
	{
		return m_panelIn.overflowCount();
	}

	void Hardware::processUC()
	{
		// Deliver queued panel input to firmware over UART2 RX. The existing
		// release/acquire pending count is a counted-work wake, not a second dirty
		// bit: a racing producer can make us defer once, but the count cannot clear
		// until this single consumer drains the published packet.
		// Do not let input mutate the bootstrap machine and then disappear when the
		// coherent project images are published. Queues remain intact until restore.
		const bool projectRestorePending =
			m_pendingFlashRestoreActive.load(std::memory_order_acquire);
		if(!projectRestorePending && m_panelIn.hasPending())
		{
			PanelInputQueue::DrainBuffer panelInput;
			const auto availablePackets = m_uc.availablePanelRxBytes() / 2;
			const auto panelInputCount = m_panelIn.drain(panelInput, availablePackets);
			for(size_t i = 0; i < panelInputCount; ++i)
			{
				const auto& packet = panelInput[i];
				// This thread is the only Sim UART producer, and drain was capped to the
				// space sampled above, so both bytes are guaranteed to fit together.
				m_uc.queuePanelRx(packet.row);
				m_uc.queuePanelRx(packet.mask);
			}
		}

		// The queues and transfer state publish their own positions and state.
		// Avoid entering MIDI arbitration when every source is idle; a producer
		// racing this observation is visible at the next instruction boundary.
		const bool transferActive = m_midiSysexTransfer.ownsMidiWire();
		if(!projectRestorePending && (transferActive || m_midiInByteCursor != 0
			|| !m_midiIn.empty()
			|| m_realtimeMidiIn.hasPending()))
			pumpMidiIngress();

		// Drive DSP2's HI08 HREQ into the ColdFire external IRQ4 BEFORE stepping the CPU, so the
		// interrupt this pump raises is visible to the instruction m_uc.exec() runs (SIM interrupts
		// are injected inside exec()). See pumpDsp2HostRequest.
		if(!isMonomachine()
			|| m_schedulerHostPumpDirty.load(std::memory_order_acquire))
			pumpDsp2HostRequest();

		const auto deltaCycles = m_uc.exec();
		if(transferActive)
		{
			m_midiSysexTransfer.service(deltaCycles,
				m_midiInByteCursor == 0
					&& m_realtimeMidiIn.sizeBefore(
						m_midiSysexTransfer.realtimeWriteBoundary()) == 0,
				m_uc);
		}

		m_schedUcCyclesDone += deltaCycles;
	}

	void Hardware::pumpDsp2HostRequest()
	{
		if(isMonomachine())
		{
			// The settled MM path executes millions of ColdFire instructions between meaningful
			// host-port edges. Keep that overwhelmingly common clean check read-only; reserve the
			// cache-line-writing RMW for a producer/consumer/ICR wake. A wake racing the exchange
			// remains set for the next instruction, so no event can be lost.
			if(!m_schedulerHostPumpDirty.load(std::memory_order_acquire))
				return;
			if(!m_schedulerHostPumpDirty.exchange(false, std::memory_order_acq_rel))
				return;
		}

		// Match MAME's DSP2 HI08 HREQ to ColdFire IRQ4 wiring. Continuously
		// drain HOTX into the bounded host-side queue and assert HREQ at its
		// configured threshold.
		//
		// Use the public MAME driver's bounded queue and request threshold so host
		// traffic remains ordered during startup and normal operation.
		static constexpr size_t g_hostRxIrqMinWords = 3;	// MAME set_host_rx_irq_min_words(3)
		static constexpr size_t g_maxUcQueuedWords  = 16;	// bound on the host-side queue depth

		// MAME drains both DSP transmit paths continuously; only the HREQ-to-IRQ4
		// wire is DSP2-specific. Drain the mixer path as well so its transmit
		// register cannot remain full.
		uint32_t mixerMoved = 0;
		if(m_dspMixer.booted())
			mixerMoved = m_dspMixer.pumpHostRx(g_maxUcQueuedWords);

		if(!m_dspProducer.booted())
			return;	// pre-boot: DSP2 is not producing; IRQ4 stays deasserted (reset default)

		const uint32_t producerMoved = m_dspProducer.pumpHostRx(g_maxUcQueuedWords);

		auto& hdi = m_uc.getHdi08Dsp2();
		const bool rreq = (hdi.icr() & mc68k::Hdi08::Rreq) != 0;	// ColdFire enabled receive requests
		const bool hreq = rreq && hdi.hostRxWordsAvailable() >= g_hostRxIrqMinWords;
		(void)mixerMoved;
		(void)producerMoved;
		m_uc.getSim().setExternalIrq4(hreq);
	}

	void Hardware::notifyHostPumpStateChanged()
	{
		if(isMonomachine())
			m_schedulerHostPumpDirty.store(true, std::memory_order_release);
	}

	void Hardware::onEssiCallbackMixer()
	{
		++m_esaiFrameIndex;

		// The callback runs inside the mixer on the scheduler thread. Drain the
		// codec ring immediately so its blocking producer can never park that thread.
		schedDrainCodecOutput();
	}

	void Hardware::ensureBufferSize(const uint32_t _frames)
	{
		for(auto& audioOutput : m_audioOutputs)
		{
			if(audioOutput.size() < _frames)
				audioOutput.resize(_frames, 0);
		}
	}

	void Hardware::processAudio(const uint32_t _frames, const uint32_t /*_latency*/)
	{
		ensureBufferSize(_frames);

		// During a real host callback retain the frames that the scheduler drains
		// immediately, then copy them into the plug-in's six output channels.
		for(auto& output : m_audioOutputs)
			std::fill_n(output.data(), _frames, dsp56k::TWord(0));

		m_schedHostAudioActive = true;
		const auto trimmed = renderHostAudio(m_schedHostAudio, m_audioOutputs, _frames,
			[this](const uint32_t _chunk) { advance(_chunk); });
		m_schedHostAudioActive = false;

		// Preserve a small surplus to maintain codec continuity, but never allow
		// stale output to accumulate beyond one current host block.
		if(trimmed)
			m_schedHostAudioOverflow.fetch_add(trimmed, std::memory_order_relaxed);
	}

	void Hardware::processAudio(const synthLib::TAudioOutputs& _outputs, const uint32_t _frames, const uint32_t _latency)
	{
		processAudio(_frames, _latency);

		for(uint32_t ch = 0; ch < 2; ++ch)
		{
			if(!_outputs[ch])
				continue;
			for(uint32_t i = 0; i < _frames; ++i)
				_outputs[ch][i] = dsp56k::dsp2sample<float>(m_audioOutputs[ch][i]);
		}
	}

	void Hardware::processAudio(const synthLib::TAudioInputs& _inputs,
		const synthLib::TAudioOutputs& _outputs, const uint32_t _frames,
		const uint32_t _latency)
	{
		m_hostAudioInputs = _inputs;
		m_hostAudioInputFrames = _frames;
		m_hostAudioInputCursor = 0;
		m_hostAudioInputActive = true;
		processAudio(_outputs, _frames, _latency);
		m_hostAudioInputActive = false;
		m_hostAudioInputFrames = 0;
		m_hostAudioInputs.fill(nullptr);
	}

	// -------------------------------------------------------------------------------------------
	// The deterministic interleave scheduler advances the whole machine by
	// _machineFrames codec frames of shared machine time, on the caller's thread, with no background
	// threads. It maintains a machine clock in codec frames and, in an event-driven loop, repeatedly
	// steps whichever component (UC / DSP1 / DSP2) is furthest BEHIND the clock forward by a bounded
	// background quantum (MAME's "run each processor in large chunks, catch up at every interaction"
	// model; fine-grained UC<->DSP synchronisation happens at each HI08 access). Rates are
	// exact: one frame = g_dsp1CyclesPerEsaiFrame (2304) DSP cycles = g_ucClockHz/g_samplerate UC
	// cycles. The scheduler uses a model-specific background quantum and MAME's
	// 100k-cycle catch-up clamp.
	// -------------------------------------------------------------------------------------------
	namespace
	{
		double schedQuantumFrames(const MachineModel _model)
		{
			// MM's host traffic needs a tighter background interleave than the MD
			// path so short mailbox pulses remain visible.
			const double us = _model == MachineModel::Monomachine ? 30.0 : 125.0;
			return us * static_cast<double>(g_samplerate) / 1.0e6;				// -> codec frames
		}

		uint64_t schedClampCycles()
		{
			return 100'000;	// MAME time_catchup_max_cycles
		}

		double schedUcCyclesPerFrame()
		{
			return static_cast<double>(g_ucClockHz) / static_cast<double>(g_samplerate);
		}

	}


	double Hardware::schedDspFramePos(const uint32_t _dspIndex)
	{
		auto& d = (_dspIndex == 0) ? m_dspMixer : m_dspProducer;
		const uint64_t cyc = d.dsp().getCycles() - m_schedDspOriginCycles[_dspIndex];
		return m_schedDspOriginFrame[_dspIndex] + static_cast<double>(cyc) / static_cast<double>(g_dsp1CyclesPerEsaiFrame);
	}

	void Hardware::schedDrainCodecOutput()
	{
		// Pop everything the mixer (DSP1) ESSI1 TX produced so its blocking push
		// can never park the single scheduler thread.
		auto& out = m_dspMixer.getPeriph().getEssi1().getAudioOutputs();
		while(!out.empty())
		{
			auto frame = out.pop_front();


			if(m_schedHostAudioActive)
			{
				const bool dropped = m_schedHostAudio.emplace(
					[&frame](RealtimeHostAudioQueue::Frame& _hostFrame)
				{
					_hostFrame.fill(0);
					if(!frame.empty())
					{
						_hostFrame[0] = frame[0][0];	// AB left, ESSI1 TX0
						_hostFrame[2] = frame[0][1];	// CD left, ESSI1 TX1
						_hostFrame[4] = frame[0][2];	// EF left, ESSI1 TX2
					}
					if(frame.size() >= 2)
					{
						_hostFrame[1] = frame[1][0];	// AB right, ESSI1 TX0
						_hostFrame[3] = frame[1][1];	// CD right, ESSI1 TX1
						_hostFrame[5] = frame[1][2];	// EF right, ESSI1 TX2
					}
				});
				if(dropped)
					m_schedHostAudioOverflow.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	bool Hardware::schedStep()
	{
		const double ucPerFrame   = schedUcCyclesPerFrame();
		const double quantumFrames= schedQuantumFrames(m_model);
		const uint64_t clampCycles= schedClampCycles();
		const double target       = m_schedFramesTotal;

		const double ucPos = static_cast<double>(m_schedUcCyclesDone) / ucPerFrame;

		// Latch the rate-lock origin of any DSP that just became runnable (boot finished during a UC
		// step): it starts "now" (the current UC machine-time), its cycle counter ~0. From here its
		// machine-frame position tracks 2304 executed cycles per frame.
		for(uint32_t i = 0; i < 2; ++i)
		{
			auto& d = (i == 0) ? m_dspMixer : m_dspProducer;
			if(!m_schedDspOriginLatched[i] && d.booted())
			{
				m_schedDspOriginLatched[i]  = true;
				m_schedDspOriginFrame[i]    = ucPos;
				m_schedDspOriginCycles[i]   = d.dsp().getCycles();
			}
		}
		// A DSP that is not yet runnable is parked at the target so it is never chosen as the laggard.
		double dsp1Pos = m_schedDspOriginLatched[0] ? schedDspFramePos(0) : target;
		double dsp2Pos = m_schedDspOriginLatched[1] ? schedDspFramePos(1) : target;

		// MM host traffic is a flow-controlled lossless stream. Park a backlogged
		// DSP slice until the UC drains below
		// the threshold; a release clamp bounds the stall so a non-draining UC phase cannot
		// starve the codec. MD path untouched.
		const bool s_mmBackpressure = isMonomachine();
		if(s_mmBackpressure)
		{
			constexpr size_t   g_bpThresholdWords = 4;			// MAME MM host queue is 2 words deep
			constexpr uint64_t g_bpReleaseUcCycles = 200000;	// MAME backpressure clamp is 100k DSP cycles
			for(uint32_t i = 0; i < 2; ++i)
			{
				auto& d = (i == 0) ? m_dspMixer : m_dspProducer;
				double& pos = (i == 0) ? dsp1Pos : dsp2Pos;
				if(!m_schedDspOriginLatched[i] || !d.booted() || d.hostTxBacklog() <= g_bpThresholdWords)
				{
					m_mmBpSinceUcCycles[i] = 0;
					continue;
				}
				if(!m_mmBpSinceUcCycles[i])
					m_mmBpSinceUcCycles[i] = m_schedUcCyclesDone + 1;	// +1: 0 means "not stalled"
				if(m_schedUcCyclesDone - (m_mmBpSinceUcCycles[i] - 1) < g_bpReleaseUcCycles)
					pos = target;
			}
		}
		double minPos = ucPos; int who = 0;			// 0 = UC, 1 = DSP1(mixer), 2 = DSP2(producer)
		if(dsp1Pos < minPos) { minPos = dsp1Pos; who = 1; }
		if(dsp2Pos < minPos) { minPos = dsp2Pos; who = 2; }

		if(minPos >= target)
			return false;							// everything has reached the shared clock

		const double subTarget = std::min(minPos + quantumFrames, target);

		if(who == 0)
		{
			// Advance the UC toward subTarget; each processUC() runs one m_uc.exec() (and its HI08
			// callbacks, which catch the target DSP up inline). Guaranteed at least one step; clamped.
			const uint64_t clampStop = m_schedUcCyclesDone + clampCycles;
			processUC();
			while(static_cast<double>(m_schedUcCyclesDone) / ucPerFrame < subTarget
				&& m_schedUcCyclesDone < clampStop)
				processUC();
		}
		else
		{
			auto& d = (who == 1) ? m_dspMixer : m_dspProducer;
			const uint32_t idx = who - 1;
			const uint64_t startCyc  = d.dsp().getCycles();
			uint64_t targetCyc = m_schedDspOriginCycles[idx]
				+ static_cast<uint64_t>((subTarget - m_schedDspOriginFrame[idx]) * static_cast<double>(g_dsp1CyclesPerEsaiFrame));
			if(targetCyc <= startCyc)
				targetCyc = startCyc + 1;			// guarantee >=1 step of progress (float rounding)
			const uint64_t clampStop = startCyc + clampCycles;
			const uint64_t stopCyc = std::min(targetCyc, clampStop);
			if(m_schedBoundedJit)
				d.dsp().execUntilCycles(stopCyc);
			else
			{
				d.dsp().exec();
				while(d.dsp().getCycles() < stopCyc)
					d.dsp().exec();
			}
			if(who == 1)
				schedDrainCodecOutput();			// keep the mixer ESSI1 output ring shallow
		}



		return true;
	}

	void Hardware::schedCatchUpDsp(const uint32_t _dspIndex)
	{
		// MAME catch_up_elapsed_time: run the target DSP inline up to the UC's current machine time
		// (the caller's point in the boot handshake) before a host access. This is what advances the
		// DSP in fine lockstep with the UC's poll loops, so the UC's ISR/reply polls converge instead
		// of spinning while the DSP is frozen for the UC's whole background quantum. Bounded by the
		// catch-up clamp; monotone (never runs the DSP backwards or past the UC).
		const uint32_t i = _dspIndex & 1;
		if(!m_schedDspOriginLatched[i])
			return;									// not yet rate-locked (still booting) - nothing to catch up
		auto& d = (i == 0) ? m_dspMixer : m_dspProducer;

		const double ucPos = static_cast<double>(m_schedUcCyclesDone) / schedUcCyclesPerFrame();
		const double deltaFrames = ucPos - m_schedDspOriginFrame[i];
		if(deltaFrames <= 0.0)
			return;
		const uint64_t targetCyc = m_schedDspOriginCycles[i]
			+ static_cast<uint64_t>(deltaFrames * static_cast<double>(g_dsp1CyclesPerEsaiFrame));
		const uint64_t clampStop = d.dsp().getCycles() + schedClampCycles();
		// MM flow control: a host-TX-backlogged DSP does not advance in catch-up either - the
		// catch-up loops are how a DSP outruns the UC by thousands of words in the first place
		// (see the schedStep backpressure comment). MD path untouched.
		const bool s_mmBp = isMonomachine();
		while(d.dsp().getCycles() < targetCyc && d.dsp().getCycles() < clampStop
			&& (!s_mmBp || d.hostTxBacklog() <= 4))
			d.dsp().exec();
	}

	void Hardware::schedCatchUpDspToDsp(const uint32_t _consumer, const uint32_t _producer)
	{
		// Before a producer DSP enqueues a link frame into the MAME-style ESSI route,
		// consumer DSP's input ring, advance the CONSUMER to the producer's current machine time - so a
		// frame is never consumed "before" (in DSP-time) it was produced, nor an arbitrary quantum
		// late. The reentrancy guard stops the consumer's own back-channel pushes from recursing into a
		// second catch-up (they just enqueue non-blocking; that DSP is caught up at its next link frame
		// or by the scheduler). Bounded by the catch-up clamp; only ever runs a DSP forward.
		if(m_schedInLinkDelivery)
			return;
		const uint32_t c = _consumer & 1;
		const uint32_t p = _producer & 1;

		if(!m_schedDspOriginLatched[c] || !m_schedDspOriginLatched[p])
			return;
		auto& d = (c == 0) ? m_dspMixer : m_dspProducer;
		const double producerPos = schedDspFramePos(p);
		const double deltaFrames = producerPos - m_schedDspOriginFrame[c];
		if(deltaFrames <= 0.0)
		{
			return;
		}
		const uint64_t targetCyc = m_schedDspOriginCycles[c]
			+ static_cast<uint64_t>(deltaFrames * static_cast<double>(g_dsp1CyclesPerEsaiFrame));
		if(d.dsp().getCycles() >= targetCyc)
		{
			// Most link writes arrive after the consumer's ordinary scheduler slice already
			// reached this producer timestamp. The old zero-iteration path merely entered and
			// left the reentrancy guard; returning here is equivalent and avoids that hot cost.
			return;
		}
		const uint64_t clampStop = d.dsp().getCycles() + schedClampCycles();
		m_schedInLinkDelivery = true;
		const bool bpGate = isMonomachine();
		while(d.dsp().getCycles() < targetCyc && d.dsp().getCycles() < clampStop
			&& (!bpGate || d.hostTxBacklog() <= 4))
			d.dsp().exec();
		m_schedInLinkDelivery = false;
	}

	void Hardware::advance(const uint32_t _machineFrames)
	{
		m_schedFramesTotal += static_cast<double>(_machineFrames);

		while(schedStep())
		{
		}

		schedDrainCodecOutput();					// final drain (also covers a UC-only advance window)
		advanceFactoryFlashCapture();
		// Never make the emulation/audio thread wait for a UI snapshot read. If the
		// reader owns the short copy lock, the next machine interval republishes.
		m_frontPanelPublisher->tryPublish(m_frontPanel);
	}

	bool Hardware::sendMidi(const synthLib::SMidiEvent& _ev)
	{
		// Internal clock traffic and the controller's exact read-only state queries
		// do not affect the factory baseline. All other routable traffic does.
		if(_ev.source != synthLib::MidiEventSource::Internal
			&& (_ev.sysex.empty() || !automation::sysex::isReadOnlyRequest(
				m_model, _ev.sysex)))
			registerExternalInteraction();
		m_midiIn.push_back(_ev);
		return true;
	}

	template<typename InactiveObserved>
	void Hardware::pumpMidiIngressImpl(InactiveObserved&& _inactiveObserved)
	{
		const auto captureTransferBoundary = [this](size_t& _writeBoundary)
		{
			// Queued is published with release semantics after the producer boundary.
			// The acquire in ownsMidiWire() therefore makes the
			// corresponding non-atomic boundary visible before it is read here.
			if(!m_midiSysexTransfer.ownsMidiWire())
				return false;
			_writeBoundary = m_midiSysexTransfer.realtimeWriteBoundary();
			return true;
		};

		const auto pumpRealtime = [this, &captureTransferBoundary](
			bool _hasWriteBoundary, size_t _writeBoundary)
		{
			uint8_t byte = 0;
			for(;;)
			{
				if(!_hasWriteBoundary
					&& captureTransferBoundary(_writeBoundary))
					_hasWriteBoundary = true;
				if(_hasWriteBoundary
					&& m_realtimeMidiIn.sizeBefore(_writeBoundary) == 0)
					return true;
				if(!m_realtimeMidiIn.tryPeek(byte))
					return !_hasWriteBoundary;

				// startMidiSysexTransfer() may race the initial inactive observation.
				// Re-acquire the state after peeking and before UART admission. If a
				// request was published meanwhile, only bytes before its published
				// write boundary may cross. A byte whose peek overlaps publication is
				// either in that prefix or linearizes before the request; a byte
				// published after the request is necessarily rejected here.
				if(!_hasWriteBoundary
					&& captureTransferBoundary(_writeBoundary))
				{
					_hasWriteBoundary = true;
					if(m_realtimeMidiIn.sizeBefore(_writeBoundary) == 0)
						return true;
				}
				if(!m_uc.tryQueueMidiRx(byte))
					return false;
				uint8_t committed = 0;
				if(!m_realtimeMidiIn.tryPop(committed))
					return false;
			}
		};

		const auto pumpGeneralFront = [this]()
		{
			if(m_midiIn.empty())
				return false;
			// A transfer request may be published after pumpMidiIngress() first
			// observed an inactive state. Do not begin a general event after that
			// publication; once its first byte has been admitted, its cursor makes
			// the whole event the indivisible predecessor of the transfer.
			if(m_midiInByteCursor == 0 && m_midiSysexTransfer.ownsMidiWire())
				return false;
			const auto& event = m_midiIn.front();
			const auto type = static_cast<uint8_t>(event.a & 0xf0);
			const size_t byteCount = !event.sysex.empty() ? event.sysex.size()
				: (type == synthLib::M_PROGRAMCHANGE || type == synthLib::M_AFTERTOUCH)
					? 2u : type < 0xf0 ? 3u : 1u;
			while(m_midiInByteCursor < byteCount)
			{
				const auto cursor = m_midiInByteCursor;
				const uint8_t byte = !event.sysex.empty() ? event.sysex[cursor]
					: cursor == 0 ? event.a : cursor == 1 ? event.b : event.c;
				if(!m_uc.tryQueueMidiRx(byte))
					return false;
				++m_midiInByteCursor;
			}
			(void)m_midiIn.pop_front();
			m_midiInByteCursor = 0;
			return true;
		};

		// A transfer request is a wire boundary, not permission to split a MIDI
		// message already in flight. Finish only that partial general event and the
		// realtime-byte snapshot captured by startMidiSysexTransfer(); newer ingress
		// remains queued behind the transfer.
		if(m_midiSysexTransfer.ownsMidiWire())
		{
			if(m_midiInByteCursor != 0 && !pumpGeneralFront())
				return;
			(void)pumpRealtime(true, m_midiSysexTransfer.realtimeWriteBoundary());
			return;
		}
		_inactiveObserved();

		// Once a normal MIDI event has begun, finish it before switching back to the
		// semantic byte queue. At event boundaries semantic commands retain their old
		// priority over general host input.
		if(m_midiInByteCursor == 0 && !pumpRealtime(false, 0))
			return;
		while(!m_midiIn.empty())
		{
			if(!pumpGeneralFront())
				return;
			if(!pumpRealtime(false, 0))
				return;
		}
	}

	void Hardware::pumpMidiIngress()
	{
		pumpMidiIngressImpl([] {});
	}

	bool Hardware::startMidiSysexTransfer(PreparedMidiSysexTransfer& _transfer)
	{
		registerExternalInteraction();
		return m_midiSysexTransfer.start(
			_transfer, m_realtimeMidiIn.writePosition());
	}

	MidiSysexTransferProgress Hardware::getMidiSysexTransferProgress() const
	{
		return m_midiSysexTransfer.progress();
	}

}
