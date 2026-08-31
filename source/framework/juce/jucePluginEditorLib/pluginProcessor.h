#pragma once

#include "jucePluginLib/processor.h"

#include <memory>
#include <optional>
namespace mcpServer { class McpPluginServer; }

namespace jucePluginEditorLib
{
	class PluginEditorState;

	class Processor : public pluginLib::Processor
	{
	public:
		Processor(const BusesProperties& _busesProperties,
			const juce::PropertiesFile::Options& _configOptions,
			const pluginLib::Processor::Properties& _properties,
			bool _allowMcpServer = true);
		~Processor() override;

		juce::PropertiesFile::Options& getConfigOptions() { return m_configOptions; }
		juce::PropertiesFile& getConfig() { return m_config; }

		bool setLatencyBlocks(uint32_t _blocks) override;

		bool hasEditor() const override;
		juce::AudioProcessorEditor* createEditor() override;

		virtual PluginEditorState* createEditorState() = 0;
		PluginEditorState& getOrCreateEditorState();
		void destroyEditorState();
		PluginEditorState* getEditorState() const { return m_editorState.get(); }

		// Composite products cannot safely stack multiple native GPU child views
		// on every host. This per-instance override leaves the persisted standalone
		// renderer preference untouched.
		void setForceSoftwareRendererForSession(bool _force)
		{
			m_forceSoftwareRendererForSession = _force;
		}
		std::optional<bool> getForceSoftwareRendererForSession() const
		{
			return m_forceSoftwareRendererForSession;
		}

		void saveChunkData(baseLib::BinaryStream& s) override;
		bool loadCustomData(const std::vector<uint8_t>& _sourceBuffer) override;
		void loadChunkData(baseLib::ChunkReader& _cr) override;

		mcpServer::McpPluginServer* getMcpServer() const { return m_mcpServer.get(); }
		void setMcpServerEnabled(bool _enabled);

	protected:
		enum class ConfigMode
		{
			Persistent,
			Ephemeral
		};

		Processor(const BusesProperties& _busesProperties,
			const juce::PropertiesFile::Options& _configOptions,
			const pluginLib::Processor::Properties& _properties,
			bool _allowMcpServer, ConfigMode _configMode);

	private:
		juce::File initConfigFile(const juce::PropertiesFile::Options& _o) const;
		void savePluginLoadPath();
		void startMcpServer();
		void stopMcpServer();

		std::unique_ptr<PluginEditorState> m_editorState;

		juce::PropertiesFile::Options m_configOptions;
		juce::PropertiesFile m_config;

		std::vector<uint8_t> m_editorStateData;
		std::optional<bool> m_forceSoftwareRendererForSession;

		std::unique_ptr<mcpServer::McpPluginServer> m_mcpServer;
	};
}
