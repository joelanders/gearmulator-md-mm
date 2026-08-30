#include "mdAutomationTestSupport.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
	using namespace mdAutomationTest;

	uint64_t appendDigest(uint64_t _digest,
		const md::automation::ControlChange& _message)
	{
		for(const auto byte : _message)
		{
			_digest ^= byte;
			_digest *= 1099511628211ull;
		}
		return _digest;
	}

	void verifyFirmwareSoak(Harness& _harness, const size_t _writeCount)
	{
		auto allParameters = parameters(_harness, false);
		std::vector<int> expected(allParameters.size(), -1);
		std::mt19937 random(0x534f414bu + static_cast<unsigned>(_harness.model));
		const auto beforeCount =
			_harness.controller.getTransmittedAutomationChangeCount();
		const auto beforeMidi = _harness.telemetry();
		for(size_t write = 0; write < _writeCount; ++write)
		{
			const auto index = static_cast<size_t>(random() % allParameters.size());
			const auto value = static_cast<int>(random() & 0x7f);
			hostWrite(*allParameters[index], value);
			expected[index] = value;
			if((write & 7) == 7)
				_harness.process(4);
		}

		// Firmware echoes and parameter normalization arrive asynchronously. Require
		// the final Level values to remain correct across a sustained drain window,
		// rather than sampling the cache at one scheduler-dependent instant.
		auto levelsMatch = [&]()
		{
			for(size_t index = 0; index < allParameters.size(); ++index)
			{
				if(expected[index] >= 0
					&& allParameters[index]->getDescription().name == "Level"
					&& allParameters[index]->getUnnormalizedValue() != expected[index])
					return false;
			}
			return true;
		};
		size_t stableBlocks = 0;
		for(size_t block = 0; block < 3000 && stableBlocks < 128; ++block)
		{
			_harness.process(1);
			stableBlocks = levelsMatch() ? stableBlocks + 1 : 0;
		}
		if(stableBlocks != 128)
		{
			for(size_t index = 0; index < allParameters.size(); ++index)
			{
				if(expected[index] >= 0
					&& allParameters[index]->getDescription().name == "Level"
					&& allParameters[index]->getUnnormalizedValue() != expected[index])
					throw std::runtime_error("soak final Level did not settle for track "
						+ std::to_string(allParameters[index]->getPart() + 1)
						+ ": expected " + std::to_string(expected[index])
						+ ", observed " + std::to_string(
							allParameters[index]->getUnnormalizedValue()));
			}
			throw std::runtime_error(
				"soak final Level values did not remain stable");
		}

		const auto afterMidi = _harness.telemetry();
		const auto afterCount =
			_harness.controller.getTransmittedAutomationChangeCount();
		if(afterCount != beforeCount + _writeCount)
			throw std::runtime_error("soak transmitted "
				+ std::to_string(afterCount - beforeCount) + " of "
				+ std::to_string(_writeCount) + " explicit host writes");
		require(afterMidi.consumed >= beforeMidi.consumed + _writeCount * 3,
			"firmware did not consume the complete soak stream");
		require(afterMidi.overflows == beforeMidi.overflows,
			"soak overflowed firmware MIDI RX");
		require(afterMidi.contentionDrops == beforeMidi.contentionDrops,
			"soak dropped firmware output on controller lock contention");
		require(afterMidi.capacityDrops == beforeMidi.capacityDrops,
			"soak dropped firmware output at controller queue capacity");
	}

	void verifyModel(const md::MachineModel _model, const size_t _writeCount)
	{
		Harness harness(_model);
		if(!harness.hasLocalFirmware())
		{
			std::cout << "mdAutomationSoakTest: SKIP " << modelName(_model)
				<< " (firmware unavailable)\n";
			return;
		}
		harness.prepare();
		require(harness.synchronize(), "soak firmware synchronization timed out");
		// Synchronization completes once the authoritative dumps are applied, while
		// unrelated startup MIDI can still be in flight. Establish a quiet baseline
		// so this steady-state test does not depend on another suite draining it.
		harness.process(256);
		verifyFirmwareSoak(harness, _writeCount);
		std::cout << "mdAutomationSoakTest: " << modelName(_model)
			<< " PASS (" << _writeCount << " writes)\n";
	}

	void verifyConcurrentIsolation(const size_t _writeCount)
	{
		Harness mdHarness(md::MachineModel::Machinedrum);
		Harness mmHarness(md::MachineModel::Monomachine);
		if(!mdHarness.hasLocalFirmware() || !mmHarness.hasLocalFirmware())
		{
			std::cout << "mdAutomationSoakTest: SKIP multi-instance"
				" (firmware unavailable)\n";
			return;
		}
		mdHarness.prepare();
		mmHarness.prepare();
		std::atomic<bool> start{false};
		std::exception_ptr mdError;
		std::exception_ptr mmError;
		auto run = [&start, _writeCount](Harness& _harness,
			std::exception_ptr& _error, const uint32_t _seed)
		{
			try
			{
				while(!start.load(std::memory_order_acquire))
					std::this_thread::yield();
				require(_harness.synchronize(),
					"concurrent instance failed to synchronize");
				auto allParameters = parameters(_harness, false);
				allParameters.erase(std::remove_if(allParameters.begin(),
					allParameters.end(), [](const pluginLib::Parameter* const _parameter)
					{
						return _parameter->getDescription().name != "Level";
					}), allParameters.end());
				require(!allParameters.empty(),
					"concurrent instance has no stable Level parameters");

				std::mt19937 random(_seed);
				const auto beforeCount =
					_harness.controller.getTransmittedAutomationChangeCount();
				const auto beforeMidi = _harness.telemetry();
				auto expectedDigest =
					_harness.controller.getTransmittedAutomationDigest();
				for(size_t write = 0; write < _writeCount; ++write)
				{
					const auto index =
						static_cast<size_t>(random() % allParameters.size());
					const auto value = static_cast<int>(random() & 0x7f);
					const auto& description = allParameters[index]->getDescription();
					const auto encoded = md::automation::encodeParameterChange(
						_harness.model,
						{description.page, allParameters[index]->getPart(),
							description.index, static_cast<uint8_t>(value)},
						_harness.controller.getAutomationBaseChannel());
					require(encoded.has_value(),
						"concurrent expected automation change did not encode");
					expectedDigest = appendDigest(expectedDigest, *encoded);
					hostWrite(*allParameters[index], value);
					if((write & 7) == 7)
						_harness.process(4);
				}
				_harness.process(192);

				require(_harness.controller.getTransmittedAutomationChangeCount()
					== beforeCount + _writeCount,
					"concurrent instance lost or gained automation writes");
				require(_harness.controller.getTransmittedAutomationDigest()
					== expectedDigest,
					std::string("concurrent ") + modelName(_harness.model)
					+ " automation message sequence was contaminated");
				const auto snapshot =
					_harness.controller.createAutomationSnapshot();
				require(snapshot.size() == 6
					+ _harness.audioProcessor.getParameters().size() * 4,
					std::string("concurrent ") + modelName(_harness.model)
					+ " snapshot shape was contaminated");
				const auto afterMidi = _harness.telemetry();
				require(afterMidi.overflows == beforeMidi.overflows,
					"concurrent instance overflowed firmware MIDI RX");
				require(afterMidi.contentionDrops == beforeMidi.contentionDrops,
					"concurrent instance dropped firmware output on contention");
				require(afterMidi.capacityDrops == beforeMidi.capacityDrops,
					"concurrent instance dropped firmware output at capacity");
			}
			catch(...)
			{
				_error = std::current_exception();
			}
		};

		std::thread mdThread(run, std::ref(mdHarness), std::ref(mdError),
			0x4d444d44u);
		std::thread mmThread(run, std::ref(mmHarness), std::ref(mmError),
			0x4d4d4d4du);
		start.store(true, std::memory_order_release);
		mdThread.join();
		mmThread.join();
		if(mdError)
			std::rethrow_exception(mdError);
		if(mmError)
			std::rethrow_exception(mmError);
		std::cout << "mdAutomationSoakTest: concurrent MD/MM PASS ("
			<< _writeCount << " writes per instance)\n";
	}
}

int main(const int _argc, const char* const* _argv)
{
	juce::ScopedJuceInitialiser_GUI juce;
	try
	{
		size_t soakWrites = 256;
		size_t multiWrites = 128;
		bool mdOnly = false;
		bool mmOnly = false;
		bool noMulti = false;
		bool multiOnly = false;
		for(int argument = 1; argument < _argc; ++argument)
		{
			const std::string value(_argv[argument]);
			if(value == "--md")
				mdOnly = true;
			else if(value == "--mm")
				mmOnly = true;
			else if(value == "--no-multi")
				noMulti = true;
			else if(value == "--multi-only")
				multiOnly = true;
			else if(value.rfind("--soak-writes=", 0) == 0)
				soakWrites = static_cast<size_t>(std::stoul(value.substr(14)));
			else if(value.rfind("--multi-writes=", 0) == 0)
				multiWrites = static_cast<size_t>(std::stoul(value.substr(15)));
			else
				require(false, "unknown argument: " + value);
		}
		require(soakWrites > 0 && multiWrites > 0,
			"write counts must be positive");
		if(!multiOnly)
		{
			if(!mmOnly)
				verifyModel(md::MachineModel::Machinedrum, soakWrites);
			if(!mdOnly)
				verifyModel(md::MachineModel::Monomachine, soakWrites);
		}
		if(!noMulti && !mdOnly && !mmOnly)
			verifyConcurrentIsolation(multiWrites);
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdAutomationSoakTest: " << error.what() << '\n';
		return 1;
	}
}
