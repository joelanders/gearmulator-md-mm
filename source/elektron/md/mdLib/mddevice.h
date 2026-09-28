#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "mdhardware.h"
#include "mdplusdrive.h"
#include "mdsyseximport.h"

#include "synthLib/device.h"

namespace md
{
	struct DevicePreparedStateTestAccess;

	class Device : public synthLib::Device
	{
	public:
		enum class ProjectStateRestoreStatus
		{
			Idle,
			Preparing,
			Initializing,
			Finalizing,
			Failed
		};

		class PreparationContext
		{
		public:
			PreparationContext(const PreparationContext&) = delete;
			PreparationContext& operator=(const PreparationContext&) = delete;
			~PreparationContext() = default;

		private:
			friend class Device;
			friend struct DevicePreparedStateTestAccess;

			PreparationContext(const synthLib::DeviceCreateParams& _params,
				MachineModel _model)
				: m_romData(_params.romData)
				, m_romName(_params.romName)
				, m_homePath(_params.homePath)
				, m_model(_model)
			{
			}

			// Preparation may continue after the originating Device is replaced, so this
			// context must own ROM bytes supplied in DeviceCreateParams. That adds exactly
			// params.romData.size() persistent bytes (8 MiB for an in-memory MD/MM ROM,
			// zero for the normal filesystem-discovery path). Capturing the shared context
			// itself remains O(1); changing Hardware/Rom to shared immutable storage is a
			// separate, wider memory-layout change.
			const std::vector<uint8_t> m_romData;
			const std::string m_romName;
			const std::string m_homePath;
			const MachineModel m_model;
		};

		class PreparedState
		{
		public:
			PreparedState(const PreparedState&) = delete;
			PreparedState& operator=(const PreparedState&) = delete;
			PreparedState(PreparedState&&) noexcept = default;
			PreparedState& operator=(PreparedState&&) noexcept = default;
			~PreparedState();

		private:
			friend class Device;
			friend struct DevicePreparedStateTestAccess;

			PreparedState(std::shared_ptr<const PreparationContext> _context,
				std::unique_ptr<Hardware> _hardware, const bool _containsFlash)
				: m_context(std::move(_context)), m_hardware(std::move(_hardware))
				, m_containsFlash(_containsFlash)
			{
			}

			std::shared_ptr<const PreparationContext> m_context;
			std::unique_ptr<Hardware> m_hardware;
			// Virtual +Drive responder owned by the prepared machine so the
			// drive is present from its first instruction (boot probe) and
			// any image it formats during preparation carries over at
			// commit. Adopted by the Device on commit.
			std::optional<PlusDrive> m_plusDrive;
			bool m_containsFlash = false;
			bool m_committed = false;
			// User RAM (MD kits/patterns/songs) of the machine this
			// replacement switches away from. Set only when the replacement
			// runs an OS that must initialize itself: that first-run
			// factory initialization reformats the kit store in RAM, so the
			// settled reboot reinstates these bytes instead of the
			// reformatted ones.
			std::vector<uint8_t> m_preservedUserRam;
			// OS the replacement boots: 0 for the legacy stock path, else
			// the registered variant fingerprint. Committing copies it to
			// Device::m_liveVariantFingerprint so project saves encode
			// against the matching base image.
			uint64_t m_osVariantFingerprint = 0;
		};

		Device(const synthLib::DeviceCreateParams& _params,
			const std::vector<uint8_t>& _initialPatchRam = {});
		// Best-effort persistence for a MIDI-upgraded OS image (see the
		// implementation): teardown thread only, never the audio thread.
		~Device() override;

		float getSamplerate() const override;
		bool isValid() const override;
		bool getState(std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		bool setState(const std::vector<uint8_t>& _state, synthLib::StateType _type) override;
		bool supportsStateTransactions() const override { return true; }
		std::unique_ptr<synthLib::Device::StateTransaction> beginStateTransaction(
			std::shared_ptr<const std::vector<uint8_t>> _state,
			synthLib::StateType _type) override;
		bool finishStateTransaction(synthLib::Device::StateTransaction& _transaction) override;
		std::shared_ptr<const PreparationContext> getPreparationContext() const
		{
			return m_preparationContext;
		}
		static std::unique_ptr<PreparedState> prepareState(
			std::shared_ptr<const PreparationContext> _context,
			const std::vector<uint8_t>& _state, synthLib::StateType _type,
			const FactoryFlashSnapshot& _factoryFlash = {},
			std::string* _error = nullptr,
			// Optional panel hold (e.g. FUNCTION for BOOT MODE) seeded into the
			// replacement machine's panel UART RX FIFO before its first step.
			const std::optional<PanelPacket>& _bootHold = {},
			// Replaces the state's own user RAM when non-empty: the settled
			// reboot after an OS's first-run initialization must run the
			// user's kits, not the factory kit store the initialization
			// just wrote (see copyPreservedUserRam).
			const std::vector<uint8_t>& _userRamOverride = {});
		// A sparse UW state cannot be validated without its matching factory
		// baseline. Keep the live Hardware authoritative while an isolated candidate
		// performs first-run initialization, then cold-boot the validated images.
		bool hasDeferredStateRestore() const
		{
			return m_restoreStatus == ProjectStateRestoreStatus::Initializing
				|| m_restoreStatus == ProjectStateRestoreStatus::Finalizing;
		}
		uint64_t deferredStateGeneration() const { return m_deferredStateGeneration; }
		std::unique_ptr<PreparedState> takeFinishedDeferredState(uint64_t& _generation);
		static std::unique_ptr<PreparedState> makeDeferredStateReboot(
			PreparedState& _validated);
		// Requires exclusive access to this Device. A successful exchange leaves the
		// retired Hardware in _prepared so its destruction can happen after the
		// caller releases any process/control lock.
		bool commitPreparedState(PreparedState& _prepared);
		bool commitDeferredStateRestore(PreparedState& _prepared, uint64_t _generation);
		bool rejectDeferredStateRestore(uint64_t _generation, std::string _error);
		// OS variant switch: prepares a fresh machine booting the registered
		// variant image, carrying the live +Drive user sectors (at/above
		// g_variantSplitAddress) across like a real OS upgrade preserves them.
		// Pass explicit user sectors instead (e.g. from a split sidecar import)
		// to seed from elsewhere; empty means the live machine. Returns null
		// with _error set when the variant is unknown or invalid. The caller
		// commits via commitPreparedState; the editor observes the replacement
		// through the hardware epoch. Requires exclusive access like commit.
		// The static form snapshots the live machine first (flash/patch RAM
		// copies taken under the caller's lock) so the slow replacement boot
		// runs without holding any device lock; the member form is a shortcut
		// for lock-free owners (tests).
		static std::unique_ptr<PreparedState> prepareOsVariantBoot(
			std::shared_ptr<const PreparationContext> _context,
			const std::vector<uint8_t>& _liveFlash,
			const std::vector<uint8_t>& _livePatchRam,
			uint64_t _fingerprint,
			const std::vector<uint8_t>& _userSectors,
			std::string* _error = nullptr);
		std::unique_ptr<PreparedState> prepareOsVariantBoot(uint64_t _fingerprint,
			const std::vector<uint8_t>& _userSectors, std::string* _error = nullptr);
		// Default reboot: prepares a fresh machine through the same legacy
		// resolution as fresh construction (validated factory cache, then a
		// persisted MIDI-upgraded image, stock ROM bytes otherwise). This is
		// the "no variant selected" entry of the OS switcher. Same commit,
		// locking, and static/member split as prepareOsVariantBoot.
		static std::unique_ptr<PreparedState> prepareDefaultBoot(
			std::shared_ptr<const PreparationContext> _context,
			const std::vector<uint8_t>& _livePatchRam,
			std::string* _error = nullptr);
		std::unique_ptr<PreparedState> prepareDefaultBoot(
			std::string* _error = nullptr);
		ProjectStateRestoreStatus projectStateRestoreStatus() const { return m_restoreStatus; }
		const std::string& projectStateRestoreError() const { return m_restoreError; }
		const std::string& plusDriveError() const noexcept { return m_plusDriveError; }
		// The processor's factory-initialization service used to reinstate a
		// withheld user RAM here on the settled reboot. The host now owns the
		// authoritative kit store outright, so there is nothing left to
		// reinstate; kept so the service still compiles while it is removed.
		void clearPreservedUserRam() { }
		// The registered variant this machine is running, or 0 for stock.
		uint64_t liveVariantFingerprint() const { return m_liveVariantFingerprint; }
		// The host's authoritative copy of the user's kits, kept outside the
		// machine so a firmware that reformats the store it finds at startup
		// costs a restore rather than the data.
		const std::vector<uint8_t>& canonicalUserRam() const
		{
			return m_canonicalUserRam;
		}
		// Puts the authoritative kit store back if the machine's copy was
		// wiped at startup. Safe to call repeatedly; does nothing once the
		// machine has content of its own.
		bool restoreUserRamIfWiped();
		// True when the machine's kit store is empty but the host's is not,
		// i.e. the firmware cleared it.
		bool userRamWasWiped() const;
		// The user RAM to persist for this machine: the preserved copy while
		// an initialization is pending (the live kit store is the transient
		// factory reset it just wrote), the live RAM otherwise.
		std::vector<uint8_t> copyUserRamForPersistence() const
		{
			return m_hardware ? m_hardware->copyPatchRam() : std::vector<uint8_t>{};
		}
		bool captureFactoryFlashCachePersistence(std::string& _filename,
			FactoryFlashSnapshot& _snapshot, std::string& _error);		static bool materializeFactoryFlashCache(FactoryFlashSnapshot& _snapshot,
			const std::shared_ptr<const PreparationContext>& _context,
			std::string& _error);
		static bool writeFactoryFlashCachePersistence(const std::string& _filename,
			const std::vector<uint8_t>& _cache, std::string& _error);
		uint32_t getChannelCountIn() override;
		uint32_t getChannelCountOut() override;
		// The synchronous scheduler needs no extra host-block MIDI reserve.
		uint32_t getDefaultLatencyBlocks() const override { return 0; }
		uint32_t getInternalLatencyInputToOutput() const override
		{
			return g_hostAudioInputSafetyFrames;
		}
		bool setDspClockPercent(uint32_t _percent) override;
		uint32_t getDspClockPercent() const override;
		uint64_t getDspClockHz() const override;
		MachineModel getModel() const { return m_model; }
		bool supportsRamRecordingMode() const
		{
			return m_hardware && m_hardware->supportsRamRecordingMode();
		}
		void setRamRecordingMode(RamRecordingMode _mode);
		RamRecordingMode ramRecordingMode() const { return m_ramRecordingMode; }
		uint64_t hardwareEpoch() const { return m_hardwareEpoch; }
		// All import lifecycle calls require the owning Plugin/processor lock.
		// Payload ownership remains in TurboMidiTransfer; dialogs carry only tickets.
		std::optional<SysexImportTicket> beginUserSysexImport();
		SysexImportStartResult startUserSysexImport(const SysexImportTicket& ticket,
			PreparedMidiSysexTransfer& transfer, bool receiveModeConfirmed);
		bool cancelUserSysexImport(const SysexImportTicket& ticket, std::vector<uint8_t>& retired);
		bool resumeUserSysexImport(const SysexImportTicket& ticket, uint32_t transferId,
			size_t receiveStep, bool receiveModeConfirmed);
		bool retireUserSysexImport(const SysexImportTicket& ticket, std::vector<uint8_t>& retired);
		SysexImportProgress userSysexImportProgress() const;
		void setNativeProgramChangesEnabled(bool _enabled) { m_nativeProgramChangesEnabled = _enabled; }
		bool nativeProgramChangesEnabled() const { return m_nativeProgramChangesEnabled; }
		bool isProjectStateRestorePending() const
		{
			return m_restoreStatus == ProjectStateRestoreStatus::Preparing
				|| m_restoreStatus == ProjectStateRestoreStatus::Initializing
				|| m_restoreStatus == ProjectStateRestoreStatus::Finalizing
				|| m_hardware->isProjectStateRestorePending();
		}
		// Panel interaction remains directed at the audible live machine while a
		// replacement is prepared. It is intentionally ephemeral across the reboot.
		bool sendPanelEvent(uint8_t _command, uint8_t _argument)
		{
			return m_hardware->trySendPanelEvent(_command, _argument);
		}
		PanelInputQueueStatus getPanelInputStatus() const
		{
			return m_hardware->getPanelInputStatus();
		}
		FrontPanel getFrontPanelSnapshot() const { return m_frontPanelPublisher->read(); }
		std::shared_ptr<FrontPanelPublisher> getFrontPanelPublisher() const
		{
			return m_frontPanelPublisher;
		}
		FrontPanelPublishedState getFrontPanelPublishedState() const
		{
			return m_frontPanelPublisher->readPublishedState();
		}
		size_t drainFrontPanelLedTransitions(FrontPanelLedTransition* _output,
			size_t _capacity)
		{
			return m_frontPanelPublisher->drainLedTransitions(_output, _capacity);
		}
		FrontPanelLedTransitionStatus getFrontPanelLedTransitionStatus() const
		{
			return m_frontPanelPublisher->getLedTransitionStatus();
		}
		// Direct access for the in-process editor (option B): the front-panel LCD/LED state
		// and panel-event injection. Only valid for a local (non-bridged) device instance.
		Hardware& getHardware() { return *m_hardware; }
		const Hardware& getHardware() const { return *m_hardware; }

	protected:
		void extraLatencyChanged() override;
		void readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut) override;
		void processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, size_t _samples) override;
		bool sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response) override;

	private:
		friend struct DevicePreparedStateTestAccess;
		bool matchesUserSysexImport(const SysexImportTicket& ticket) const;

		class StateTransactionImpl final : public synthLib::Device::StateTransaction
		{
		public:
			bool prepare() override;

		private:
			friend class Device;
			StateTransactionImpl(std::shared_ptr<const PreparationContext> _context,
				std::shared_ptr<const std::vector<uint8_t>> _state,
				synthLib::StateType _type,
				FactoryFlashSnapshot _factoryFlash, uint64_t _generation,
				std::unique_ptr<PreparedState> _displaced)
				: m_context(std::move(_context)), m_state(std::move(_state)), m_type(_type)
				, m_factoryFlash(std::move(_factoryFlash))
				, m_generation(_generation), m_displaced(std::move(_displaced))
			{
			}

			std::shared_ptr<const PreparationContext> m_context;
			std::shared_ptr<const std::vector<uint8_t>> m_state;
			synthLib::StateType m_type;
			FactoryFlashSnapshot m_factoryFlash;
			uint64_t m_generation;
			std::unique_ptr<PreparedState> m_displaced;
			std::unique_ptr<PreparedState> m_prepared;
			std::string m_error;
			// Disposed with the transaction outside the realtime device lock.
			std::vector<uint8_t> m_retiredSysex;
		};

		void clearProjectStateRestore();
		void failProjectStateRestore(std::string _error);
		// Writes the nvram OS-upgrade side file when the live flash image no
		// longer matches its source ROM. Called from the destructor only.
		void persistOsUpgradeImage();
	public:
		// Snapshots the live flash image into its OS hibernation slot (the
		// registered variant it matches, else the stock slot guarded by ROM
		// validation). Makes every OS remember its own last state across
		// switches and relaunches; boot prefers hibernation over cache.
		// Refuses while a project restore is pending (transient flash) and
		// never touches the audio thread: caller holds the device lock (or
		// is the teardown thread). A half-flashed image fails validation
		// and is refused rather than persisted. The static form writes an
		// already-copied image, so callers under the realtime lock snapshot
		// first (copyFlashData) and persist after releasing it.
		static bool hibernateOsImage(const std::string& _homePath,
			MachineModel _model, const std::vector<uint8_t>& _flash,
			const std::vector<uint8_t>& _romData, const std::string& _romName,
			std::string* _error = nullptr,
			const std::vector<uint8_t>& _patchRam = {});
		bool hibernateCurrentOs(std::string* _error = nullptr);
	private:

		const MachineModel m_model;
		std::shared_ptr<FrontPanelPublisher> m_frontPanelPublisher;
		std::shared_ptr<const PreparationContext> m_preparationContext;
		std::unique_ptr<Hardware> m_hardware;
		// Virtual +Drive (P4): device-lifetime responder; re-attached to
		// every replacement Hardware (each has its own Sim). Backing file is
		// machine-local and shared across OSes; hibernation never touches it.
		std::optional<PlusDrive> m_plusDrive;
		std::string m_plusDriveError;
		bool attachPlusDrive(Hardware& _hardware, const std::string& _homePath,
			MachineModel _model, std::string* _error = nullptr);
		// Prepared-state variant (static: prepare functions have no Device
		// instance). Same behavior, owned by the PreparedState.
		static bool attachPreparedPlusDrive(PreparedState& _prepared,
			std::string* _error = nullptr);
		std::unique_ptr<PreparedState> m_deferredPreparedState;
		std::shared_ptr<const std::vector<uint8_t>> m_requestedState;
		synthLib::StateType m_requestedStateType = synthLib::StateTypeGlobal;
		ProjectStateRestoreStatus m_restoreStatus = ProjectStateRestoreStatus::Idle;
		std::string m_restoreError;
		bool m_nativeProgramChangesEnabled = true;
		RamRecordingMode m_ramRecordingMode = RamRecordingMode::Original;
		std::string m_mdFlashCacheFilename;
		uint64_t m_hardwareEpoch = 0;
		std::vector<uint8_t> m_preservedUserRam;
		// OS the live machine boots: 0 for the legacy stock path, else the
		// registered variant fingerprint. Set at construction and on every
		// prepared-state commit; project saves encode their flash overlay
		// against the matching base image so restores validate.
		uint64_t m_liveVariantFingerprint = 0;
		// The host's authoritative kit store, never handed to the firmware for
		// safekeeping. Kits are machine state, not OS content, so this survives
		// an OS change and can restore a store the firmware reformatted.
		std::vector<uint8_t> m_canonicalUserRam;
		const uint64_t m_sysexDeviceId;
		SysexImportTicket m_sysexTicket;
		bool m_sysexStarted = false;
		bool m_sysexPendingCancelled = false;
		uint64_t m_deferredStateGeneration = 0;
	};
}
