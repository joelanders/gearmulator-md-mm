#include <chrono>

#include "fakeAudioDevice.h"
#include "pluginHost.h"
#include "logger.h"
#include "baseLib/commandline.h"
#include "baseLib/filesystem.h"

#include <cmath>
#include <set>
#include <vector>

namespace
{
	bool runAutomationStateSmoke(AudioProcessor& _processor, String& _error)
	{
		std::vector<AudioProcessorParameter*> parameters;
		std::set<String> parameterIds;
		for(auto* const parameter : _processor.getParameters())
		{
			if(!parameter->isAutomatable())
				continue;
			auto* const hosted =
				dynamic_cast<HostedAudioProcessorParameter*>(parameter);
			if(hosted == nullptr)
			{
				_error = "automatable wrapper parameter has no stable ID";
				return false;
			}
			if(!parameterIds.insert(hosted->getParameterID()).second)
			{
				_error = "duplicate automatable parameter ID: "
					+ hosted->getParameterID();
				return false;
			}
			parameters.push_back(parameter);
		}
		if(parameters.size() < 8)
		{
			_error = "fewer than eight automatable parameters were exposed";
			return false;
		}
		parameters.resize(std::min<size_t>(parameters.size(), 8));

		std::vector<float> expected;
		expected.reserve(parameters.size());
		for(size_t index = 0; index < parameters.size(); ++index)
		{
			const auto steps = parameters[index]->getNumSteps();
			if(steps < 2)
			{
				_error = "automatable parameter has fewer than two steps";
				return false;
			}
			// Use an exactly representable parameter step. Hosted VST3 parameters may
			// echo the caller's unquantized float until state is reloaded, even though
			// the plug-in correctly stores the nearest discrete value.
			const auto ordinal = std::min<int>(static_cast<int>(index + 1),
				steps - 1);
			const auto value = static_cast<float>(ordinal)
				/ static_cast<float>(steps - 1);
			parameters[index]->setValue(value);
			parameters[index]->setValue(value); // repeated host points are significant
			expected.push_back(parameters[index]->getValue());
		}

		MemoryBlock state;
		_processor.getStateInformation(state);
		if(state.isEmpty())
		{
			_error = "wrapper returned empty project state";
			return false;
		}
		for(auto* const parameter : parameters)
			parameter->setValue(0.0f);
		_processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
		for(size_t index = 0; index < parameters.size(); ++index)
		{
			const auto observed = parameters[index]->getValue();
			if(std::abs(observed - expected[index]) > 0.0001f)
			{
				auto* const hosted = dynamic_cast<HostedAudioProcessorParameter*>(
					parameters[index]);
				_error = "wrapper state did not restore automation parameter "
					+ hosted->getParameterID() + ": expected "
					+ String(expected[index], 7) + ", observed "
					+ String(observed, 7);
				return false;
			}
		}
		return true;
	}
}

class JuceAppLifetimeObjects
{
public:
	JuceAppLifetimeObjects()
	{
		MessageManager::getInstance();
	}
	~JuceAppLifetimeObjects()
	{
        DeletedAtShutdown::deleteAll();
		MessageManager::deleteInstance();
	}
private:
	JUCE_DECLARE_NON_COPYABLE(JuceAppLifetimeObjects)
	JUCE_DECLARE_NON_MOVEABLE(JuceAppLifetimeObjects)
};

int main(const int _argc, char* _argv[])
{
	baseLib::CommandLine cmdLine(_argc, _argv);

	StdoutLogger logger;

	auto error = [](const String& _msg) -> int
	{
		Logger::writeToLog("Error: " + _msg);
		Logger::writeToLog("Usage:\n"
			"pluginTester -plugin <pathToPlugin> [-seconds n -blocks n -blocksize n -samplerate x -forever -repeat n -automation-smoke]");
		return 1;
	};

	const auto repeatCount = cmdLine.getInt("repeat", 1);
	try
	{
	  for (int repeatIdx = 0; repeatIdx < repeatCount; ++repeatIdx)
	  {
		if (repeatCount > 1)
		{
			char msg[64];
			(void)snprintf(msg, sizeof(msg), "=== Repeat %d / %d ===", repeatIdx + 1, repeatCount);
			Logger::writeToLog(msg);
		}

	    ConsoleApplication app;

		std::string pluginPathName = cmdLine.get("plugin");

		if (pluginPathName.empty())
		{
			return error("No plugin specified");
		}

	    {
		    // juce wants the folder for a VST3/LV2 plugin instead of the actual file
		    const auto lowercase = baseLib::filesystem::lowercase(pluginPathName);

		    auto start = lowercase.find(".vst3");
		    if (start == std::string::npos)
				start = lowercase.find(".lv2");
		    if (start == std::string::npos)
			    start = lowercase.find(".component");
		    if (start == std::string::npos)
			    start = lowercase.find(".vst");

		    if (start != std::string::npos)
		    {
			    auto slash = pluginPathName.find_first_of("\\/", start);

			    if (slash != std::string::npos)
				    pluginPathName = pluginPathName.substr(0, slash);
		    }
	    }

	    JuceAppLifetimeObjects jalto;

	    CommandLinePluginHost pluginHost;

		const auto& formatManager = pluginHost.getFormatManager();

		PluginDescription desc;

		for (int i = 0; i < formatManager.getNumFormats(); ++i)
		{
			auto* format = formatManager.getFormat(i);

			if (!format)
				continue;

			Logger::writeToLog("Attempt to load plugin as type " + format->getName());

		    KnownPluginList plugins;

			OwnedArray<PluginDescription> typesFound;
			plugins.scanAndAddFile(pluginPathName, true,typesFound, *format);

			const auto types = plugins.getTypes();

			if (types.isEmpty())
				continue;

			desc = types.getFirst();
			break;
		}

		if (desc.fileOrIdentifier.isEmpty())
			return error("Failed to find plugin " + pluginPathName);

	    if (!pluginHost.loadPlugin(desc))
			return error("Failed to load plugin " + pluginPathName);

		if(cmdLine.contains("automation-smoke"))
		{
			String smokeError;
			if(!runAutomationStateSmoke(*pluginHost.getCurrentProcessor(), smokeError))
				return error("Automation/state smoke failed: " + smokeError);
			Logger::writeToLog("Automation/state smoke PASS");
		}

		FakeAudioIODevice audioDevice;

		const uint32_t numIns = pluginHost.getCurrentProcessor()->getTotalNumInputChannels();
		const uint32_t numOuts = pluginHost.getCurrentProcessor()->getTotalNumOutputChannels();

		const auto blocksize = cmdLine.getInt("blocksize", 512);
		const auto samplerate = cmdLine.getFloat("samplerate", 48000.0f);

		auto res = audioDevice.open(numIns, numOuts, samplerate, blocksize);

		if (res.isNotEmpty())
			return error("Failed to open audio device: " + res);

		audioDevice.start(&pluginHost);

		const auto forever = cmdLine.contains("forever");

		if (forever)
		{
			uint64_t blockCount = 0;
			uint64_t sr = static_cast<uint64_t>(samplerate);

			uint64_t lastMinutes = 0;

			using Clock = std::chrono::high_resolution_clock;

			const auto tBegin = Clock::now();

			while (true)
			{
				audioDevice.processAudio();
				++blockCount;

				auto formatDuration = [](const uint64_t _seconds) -> std::string
				{
					char temp[64];
					const auto minutes = _seconds / 60;
					const auto hours = minutes / 60;
					const auto s = _seconds - minutes * 60;
					const auto m = minutes - hours * 60;
					(void)snprintf(temp, sizeof(temp), "%02uh %02um %02us", static_cast<uint32_t>(hours), static_cast<uint32_t>(m), static_cast<uint32_t>(s));
					return temp;
				};

				const auto totalSeconds = blockCount * blocksize / sr;
				const auto minutes = totalSeconds / 60;

				if (minutes != lastMinutes)
				{
					const auto t2 = Clock::now();
					const auto duration = std::chrono::duration_cast<std::chrono::seconds>(t2 - tBegin).count();

					const auto speed = static_cast<double>(totalSeconds) * 100.0 / static_cast<double>(duration);

					char temp[64];
					(void)snprintf(temp, sizeof(temp), "Processed %s, elapsed %s, speed %2.2f%%", formatDuration(totalSeconds).c_str(), formatDuration(duration).c_str(), speed);
					Logger::writeToLog(temp);
					lastMinutes = minutes;
				}
			}
		}

		const auto seconds = cmdLine.getInt("seconds", 0);
		auto blocks = cmdLine.getInt("blocks", 0);

		if (blocks && seconds)
			return error("Cannot specify both blocks and seconds");

		if (seconds)
		{
			blocks = static_cast<int>(samplerate) / blocksize * seconds;
			if (blocks == 0)
				blocks = 1;
		}

		int lastPercent = -1;

		char temp[64];

		for (int i=0; i<blocks; ++i)
		{
			audioDevice.processAudio();

			const auto percent = i * 100 / blocks;

			if (percent == lastPercent)
				continue;
			lastPercent = percent;

			(void)snprintf(temp, sizeof(temp), "Progress: %d%% (%d/%d blocks)", percent, i, blocks);
			Logger::writeToLog(temp);
		}

		(void)snprintf(temp, sizeof(temp), "Progress: %d%% (%d/%d blocks)", 100, blocks, blocks);
		Logger::writeToLog(temp);

	  } // end repeat loop
	    return 0;
	}
	catch (const std::exception& e)
	{
		juce::Logger::writeToLog(e.what());
		return 1;
	}
}
