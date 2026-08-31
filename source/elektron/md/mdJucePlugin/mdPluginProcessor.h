#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"
#include "mdLib/mdtypes.h"

#include <string_view>
#include <mutex>
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

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

	    pluginLib::Controller* createController() override;
		void saveChunkData(baseLib::BinaryStream& _stream) override;
		void loadChunkData(baseLib::ChunkReader& _reader) override;

	private:
		static BusesProperties makeBuses(md::MachineModel _model);
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer,
			bool _ephemeralConfig);

		const md::MachineModel m_model;
		const std::vector<uint8_t> m_initialPatchRam;
		std::mutex m_storageLoadMutex;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
