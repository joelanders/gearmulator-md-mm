#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"
#include "mdLib/mdtypes.h"

#include <string_view>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor
	{
	public:
		struct EphemeralConfig final {};

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
		bool importPlusDriveImage(const juce::File& _source, juce::String& _result);
		bool exportPlusDriveImage(const juce::File& _target, juce::String& _result);
		bool resetPlusDrive(juce::String& _result);
		bool rebootMachinedrum(juce::String& _result);
		bool rebootDevice() override;
		bool enablePlusDriveAutoSave(const juce::File& _target, juce::String& _result);
		void disablePlusDriveAutoSave();
		bool plusDriveAutoSaveEnabled() const;
		juce::File getPlusDriveAutoSaveFile() const;
		juce::String getPlusDrivePersistenceStatus() const;

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

		pluginLib::Controller* createController() override;
		void saveChunkData(baseLib::BinaryStream& _stream) override;
		bool loadCustomData(const std::vector<uint8_t>& _sourceBuffer) override;
		void loadChunkData(baseLib::ChunkReader& _reader) override;

	private:
		static BusesProperties makeBuses(md::MachineModel _model);
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer,
			bool _ephemeralConfig);
		bool replacePlusDrive(std::vector<uint8_t> _replacement,
			const juce::String& _operation, juce::String& _result);
		bool exportPlusDriveImageUnlocked(const juce::File& _target,
			juce::String& _result);
		void startPlusDrivePersistenceThread();
		void stopPlusDrivePersistenceThread();
		void plusDrivePersistenceLoop();

		const md::MachineModel m_model;
		const std::vector<uint8_t> m_initialPatchRam;
		std::mutex m_storageLoadMutex;
		mutable std::mutex m_plusDrivePersistenceMutex;
		std::condition_variable m_plusDrivePersistenceCv;
		std::thread m_plusDrivePersistenceThread;
		std::unique_ptr<juce::InterProcessLock> m_plusDriveInterprocessLock;
		std::string m_plusDriveAutoSavePath;
		juce::String m_plusDrivePersistenceStatus{"Project-owned +Drive"};
		bool m_plusDrivePersistenceStop = false;
		uint64_t m_plusDriveFlushRequest = 0;
		uint64_t m_plusDriveFlushedRequest = 0;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
