#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"
#include "mdLib/mdtypes.h"

#include <string_view>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mdJucePlugin
{
	class StandalonePlusDrivePersistence;

	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor,
		private juce::Timer
	{
	public:
		struct EphemeralConfig final
		{
			// Tests may route the standalone checkpoint into an isolated directory.
			// Production callers leave this empty and use the fixed user-data path.
			juce::File standalonePlusDriveFile;
			// Ephemeral processors must not read or persist a real machine's NVRAM.
			// Tests may still use normal read-only ROM discovery, or inject a ROM below.
			bool isolateDeviceStorage = true;
			std::vector<uint8_t> romData;
			std::string romName;
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
		bool importPlusDriveImage(const juce::File& _source, juce::String& _result);
		bool exportPlusDriveImage(const juce::File& _target, juce::String& _result);
		bool resetPlusDrive(juce::String& _result);
		bool rebootMachinedrum(juce::String& _result);
		bool rebootDevice() override;
		bool serviceFactoryInitialization();
		juce::String getPlusDrivePersistenceStatus() const;

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

		pluginLib::Controller* createController() override;
		void saveChunkData(baseLib::BinaryStream& _stream) override;
		bool loadCustomData(const std::vector<uint8_t>& _sourceBuffer) override;
		void loadChunkData(baseLib::ChunkReader& _reader) override;

	private:
		static BusesProperties createBusesProperties();
		bool isBusesLayoutSupported(const BusesLayout& _layout) const override;
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer,
			bool _ephemeralConfig, juce::File _standalonePlusDriveFile = {},
			bool _isolateDeviceStorage = false,
			std::vector<uint8_t> _romData = {}, std::string _romName = {});
		bool replacePlusDrive(std::vector<uint8_t> _replacement,
			const juce::String& _operation, juce::String& _result);
		bool exportPlusDriveImageUnlocked(const juce::File& _target,
			juce::String& _result);
		void initializeStandalonePlusDrivePersistence();
		bool capturePlusDrivePersistenceSnapshot(bool _includeData,
			uint64_t& _epoch, uint64_t& _generation, bool& _dirty,
			std::vector<uint8_t>& _data);
		void acknowledgePlusDrivePersistence(uint64_t _epoch, uint64_t _generation);
		void timerCallback() override;

		const md::MachineModel m_model;
		const std::vector<uint8_t> m_initialPatchRam;
		const juce::File m_standalonePlusDriveFile;
		const bool m_isolateDeviceStorage;
		const std::vector<uint8_t> m_romData;
		const std::string m_romName;
		std::mutex m_storageLoadMutex;
		juce::String m_factoryInitializationStatus;
		std::unique_ptr<StandalonePlusDrivePersistence> m_standalonePlusDrive;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
