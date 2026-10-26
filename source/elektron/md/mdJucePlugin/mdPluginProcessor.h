#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"
#include "mdLib/mdtypes.h"
#include "mdLib/mdpanel.h"
#include "synthLib/performanceReport.h"

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor,
		private juce::Timer
	{
	public:
		struct EphemeralConfig final
		{
			// Tests may explicitly isolate the emulated machine from persistent
			// factory/storage caches. A disengaged value preserves normal discovery;
			// an engaged empty value disables the device home path entirely.
			std::optional<std::string> deviceHomePath;
		};

	    AudioPluginAudioProcessor();
		explicit AudioPluginAudioProcessor(md::MachineModel _model);
		AudioPluginAudioProcessor(md::MachineModel _model, bool _allowMcpServer);
		AudioPluginAudioProcessor(md::MachineModel _model, EphemeralConfig,
			bool _allowMcpServer = false);
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer = true);
	    ~AudioPluginAudioProcessor() override;

		md::MachineModel getModel() const { return m_model; }
		static md::MachineModel getCompiledProductModel();
		static bool hasEmbeddedProductResource(std::string_view _filename);
		juce::File getInstalledFactoryStorageImage() const;
		juce::File getStorageRecoveryImage() const;
		bool loadStorageImage(const juce::File& _source, juce::String& _result);
		// OS variants, per-instance selection (OsTIrus ROM-choice model). The
		// instance fingerprint wins; the os-variant.txt home default applies
		// to fresh instances; zero means the legacy boot path (factory cache,
		// persisted MIDI upgrade, or stock ROM). Switching reboots the live
		// machine immediately, carrying user data across; importing only
		// registers a .syx file as a new variant.
		uint64_t getOsVariantFingerprint() const;
		std::string getDeviceHomePath() const;
		bool importOsVariant(const juce::File& _sysexFile, juce::String& _result);
		bool switchOsVariant(uint64_t _fingerprint, juce::String& _result);
		bool serviceFactoryInitialization();
		bool serviceProjectStateRestore();
		// Reboot the machine with FUNCTION held so firmware enters its EARLY
		// STARTUP MENU instead of booting normally. Reuses the current project
		// state; an OS can then be sent over MIDI via MIDI UPGRADE (e.g. with
		// Elektron Transfer).
		bool rebootToBootMode(juce::String& _result);
		// Plain power cycle, as on hardware: no panel button held, so the
		// machine boots normally. User RAM carries over. This is the everyday
		// reboot; rebootToBootMode() is the maintenance variant.
		bool reboot(juce::String& _result);
		// Shared power-cycle body. _hold is the panel packet presented while
		// rebooting (empty for a normal boot) and _enterBootMode selects
		// whether the replacement waits in EARLY STARTUP MENU.
		bool powerCycle(const std::optional<md::PanelPacket>& _hold,
			bool _enterBootMode, juce::String& _result);
		bool isBootModeArmed() const { return m_bootModeArmed; }
		std::string getProjectStateRestoreError();
		void setPerformanceDiagnosticsEnabled(bool _enabled);
		bool performanceDiagnosticsActive() const;
		std::string performanceDiagnosticsStatus() const;
		juce::File performanceDiagnosticsFolder() const;
		juce::File performanceDiagnosticsFile() const { return m_performanceReportFile; }
		void setRamRecordingMode(md::RamRecordingMode _mode);
		md::RamRecordingMode getRamRecordingMode() const
		{
			return static_cast<md::RamRecordingMode>(
				m_ramRecordingMode.load(std::memory_order_relaxed));
		}
		bool isRamRecordingModeAvailable();

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

	    pluginLib::Controller* createController() override;
		void saveChunkData(baseLib::BinaryStream& _stream) override;
		void loadChunkData(baseLib::ChunkReader& _reader) override;
		bool loadCustomData(const std::vector<uint8_t>& _sourceBuffer) override;

	private:
		static BusesProperties createBusesProperties();
		bool isBusesLayoutSupported(const BusesLayout& _layout) const override;
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer,
			bool _ephemeralConfig,
			std::optional<std::string> _deviceHomePath = std::nullopt);
		bool serviceDeferredStateRestore();
		bool serviceStateRestoreFailure();
		bool serviceBootModeArmed();
		void reportBootModeFailure(const std::string& _error);
		void recordStandaloneStartupDiagnostics();
		void reportProjectStateRestoreFailure(const std::string& _error);
		void timerCallback() override;

		std::unique_ptr<synthLib::PerformanceReport> m_performanceReport;
		juce::File m_performanceReportFile;
		bool m_performanceFolderError = false;
		const md::MachineModel m_model;
		const std::vector<uint8_t> m_initialPatchRam;
		const std::optional<std::string> m_deviceHomePath;
		std::mutex m_storageLoadMutex;
		std::mutex m_bootModeMutex;
		std::mutex m_osSwitchMutex;
		mutable std::mutex m_osVariantMutex;
		uint64_t m_osVariantFingerprint = 0;
		bool m_osVariantFingerprintSet = false;
		bool m_bootModeArmed = false;
		// When this machine started waiting for a first-run initialization, in
		// emulated cycles. Zero when not waiting.
		uint64_t m_factoryInitWaitStartCycles = 0;
		double m_bootModeArmedMilliseconds = 0.0;
		uint64_t m_reportedRestoreFailureGeneration = 0;
		juce::File m_startupDiagnosticsFile;
		double m_startupDiagnosticsStartMilliseconds = 0.0;
		bool m_startupDiagnosticsEnabled = false;
		std::atomic<uint8_t> m_ramRecordingMode{
			static_cast<uint8_t>(md::RamRecordingMode::Original)};
		bool m_ramRecordingModeChunkSeen = false;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
