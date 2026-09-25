#pragma once

namespace mcpServer
{
	class McpServer;
}

namespace mdJucePlugin
{
	class AudioPluginAudioProcessor;

	void registerMdRamTools(mcpServer::McpServer& _server, AudioPluginAudioProcessor& _processor);
}
