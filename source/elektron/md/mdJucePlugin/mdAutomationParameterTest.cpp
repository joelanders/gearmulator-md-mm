#include "jucePluginLib/parameterdescriptions.h"
#include "mdLib/mdautomation.h"
#include "mdRealtimeQueue.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
	void require(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return;
		std::cerr << "mdAutomationParameterTest: " << _message << '\n';
		std::exit(1);
	}

	pluginLib::ParameterDescriptions load(const char* const _filename)
	{
		const std::string path = std::string(MD_AUTOMATION_PARAMETER_DIR) + "/" + _filename;
		std::ifstream file(path, std::ios::binary);
		require(file.good(), "could not open " + path);
		return pluginLib::ParameterDescriptions(
			std::string(std::istreambuf_iterator<char>(file), {}));
	}

	void hashByte(uint64_t& _hash, const uint8_t _value)
	{
		_hash ^= _value;
		_hash *= 1099511628211ull;
	}

	void hashInteger(uint64_t& _hash, const int64_t _value)
	{
		for(unsigned int shift = 0; shift < 64; shift += 8)
			hashByte(_hash, static_cast<uint8_t>(
				static_cast<uint64_t>(_value) >> shift));
	}

	void hashString(uint64_t& _hash, const std::string& _value)
	{
		hashInteger(_hash, static_cast<int64_t>(_value.size()));
		for(const auto character : _value)
			hashByte(_hash, static_cast<uint8_t>(character));
	}

	uint64_t hostContractFingerprint(
		const pluginLib::ParameterDescriptions& _descriptions,
		const uint8_t _trackCount)
	{
		uint64_t hash = 14695981039346656037ull;
		size_t hostIndex = 0;
		for(uint8_t track = 0; track < _trackCount; ++track)
		{
			for(const auto& description : _descriptions.getDescriptions())
			{
				// This mirrors Parameter::genId() and Controller::registerParams(). Any
				// change here is a DAW project/automation compatibility decision.
				const auto id = std::to_string(description.page) + "_"
					+ std::to_string(track) + "_"
					+ std::to_string(description.index);
				const auto name = "Track " + std::to_string(track + 1) + " "
					+ description.displayName;
				hashInteger(hash, static_cast<int64_t>(hostIndex++));
				hashString(hash, id);
				hashInteger(hash, description.version);
				hashString(hash, name);
				hashInteger(hash, description.range.getStart());
				hashInteger(hash, description.range.getEnd());
				hashInteger(hash,
					description.defaultValue == pluginLib::Description::NoDefaultValue
						? 0 : description.defaultValue);
				hashInteger(hash, description.isDiscrete);
				hashInteger(hash, description.isBool);
				hashInteger(hash, description.isBipolar);
				hashInteger(hash, description.step);
			}
		}
		return hash;
	}

	void verify(const char* const _filename, const md::MachineModel _model,
		const size_t _trackParameterCount, const uint8_t _trackCount,
		const size_t _expectedHostParameterCount,
		const uint64_t _expectedHostContractFingerprint)
	{
		const auto descriptions = load(_filename);
		require(descriptions.isValid(), descriptions.getErrors());
		require(descriptions.getDescriptions().size() == _trackParameterCount,
			std::string(_filename) + " has the wrong description count");

		std::set<std::pair<uint8_t, uint8_t>> addresses;
		for(const auto& description : descriptions.getDescriptions())
		{
			require(!description.isNonPartSensitive(),
				"automation parameters must belong to firmware tracks");

			if(_model == md::MachineModel::Monomachine
				&& description.page == md::automation::monomachine::Effects)
			{
				if(description.index == 3)
					require(description.name == "EffectsDelayTime",
						"MM CC 83 must be labelled Delay Time");
				if(description.index == 4)
					require(description.name == "EffectsDelaySend",
						"MM CC 84 must be labelled Delay Send");
			}

			require(addresses.emplace(description.page, description.index).second,
				"duplicate track parameter address");
			for(uint8_t track = 0; track < _trackCount; ++track)
			{
				const md::automation::ParameterChange change{
					description.page, track, description.index, 0};
				require(md::automation::encodeParameterChange(_model, change, 0).has_value(),
					"host parameter has no MIDI mapping");
			}
		}

		require(_trackParameterCount * _trackCount
			== _expectedHostParameterCount, "host parameter count changed");
		const auto fingerprint = hostContractFingerprint(descriptions, _trackCount);
		if(fingerprint != _expectedHostContractFingerprint)
		{
			std::cerr << _filename << " host contract fingerprint is "
				<< fingerprint << '\n';
			require(false, "host parameter IDs/order/names/ranges/defaults changed");
		}
	}

	void verifyRealtimeQueue()
	{
		mdJucePlugin::RealtimeQueue<uint32_t, 8> bounded;
		for(uint32_t value = 0; value < 8; ++value)
			require(bounded.tryPush(value), "bounded queue filled too early");
		require(!bounded.tryPush(8), "bounded queue accepted an over-capacity item");
		for(uint32_t value = 0; value < 8; ++value)
		{
			uint32_t observed = 99;
			require(bounded.tryPop(observed) && observed == value,
				"bounded queue changed FIFO order");
		}
		uint32_t empty = 0;
		require(!bounded.tryPop(empty), "bounded queue popped from empty");

		constexpr uint32_t ProducerCount = 4;
		constexpr uint32_t ItemsPerProducer = 2000;
		constexpr uint32_t ItemCount = ProducerCount * ItemsPerProducer;
		mdJucePlugin::RealtimeQueue<uint32_t, 1024> concurrent;
		std::array<std::atomic<uint8_t>, ItemCount> seen{};
		std::array<std::thread, ProducerCount> producers;
		for(uint32_t producer = 0; producer < ProducerCount; ++producer)
		{
			producers[producer] = std::thread([&, producer]
			{
				for(uint32_t ordinal = 0; ordinal < ItemsPerProducer; ++ordinal)
				{
					const auto value = producer * ItemsPerProducer + ordinal;
					while(!concurrent.tryPush(value))
						std::this_thread::yield();
				}
			});
		}
		for(uint32_t count = 0; count < ItemCount;)
		{
			uint32_t value = 0;
			if(!concurrent.tryPop(value))
			{
				std::this_thread::yield();
				continue;
			}
			require(value < ItemCount, "concurrent queue returned invalid data");
			require(seen[value].fetch_add(1, std::memory_order_relaxed) == 0,
				"concurrent queue returned an item twice");
			++count;
		}
		for(auto& producer : producers)
			producer.join();
		for(const auto& count : seen)
			require(count.load(std::memory_order_relaxed) == 1,
				"concurrent queue lost an item");
	}
}

int main()
{
	verifyRealtimeQueue();
	verify("parameterDescriptions_md.json", md::MachineModel::Machinedrum,
		26, md::automation::machinedrum::TrackCount, 416, 6612241820543455307ull);
	verify("parameterDescriptions_mm.json", md::MachineModel::Monomachine,
		58, md::automation::monomachine::TrackCount, 348, 15680169437425968391ull);
	std::cout << "mdAutomationParameterTest: PASS\n";
	return 0;
}
