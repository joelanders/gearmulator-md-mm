#pragma once

#include "jucePluginEditorLib/pluginProcessor.h"
#include "mdLib/mdtypes.h"

#include <string_view>
#include <mutex>
#include <vector>

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor,
		private juce::Timer
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
		bool serviceFactoryInitialization();
		std::string getProjectStateRestoreError();

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

	    pluginLib::Controller* createController() override;

	private:
		static BusesProperties makeBuses(md::MachineModel _model);
		AudioPluginAudioProcessor(md::MachineModel _model,
			std::vector<uint8_t> _initialPatchRam, bool _allowMcpServer,
			bool _ephemeralConfig);
		bool serviceDeferredStateRestore();
		bool serviceStateRestoreFailure();
		void reportProjectStateRestoreFailure(const std::string& _error);
		void timerCallback() override;

		const md::MachineModel m_model;
		const std::vector<uint8_t> m_initialPatchRam;
		std::mutex m_storageLoadMutex;
		uint64_t m_reportedRestoreFailureGeneration = 0;
		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
