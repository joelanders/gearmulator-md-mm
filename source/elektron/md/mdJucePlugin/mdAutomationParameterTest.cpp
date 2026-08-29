#include "jucePluginLib/parameterdescriptions.h"
#include "mdLib/mdautomation.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

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

	void verify(const char* const _filename, const md::MachineModel _model,
		const size_t _trackParameterCount, const uint8_t _trackCount,
		const size_t _expectedHostParameterCount)
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
	}
}

int main()
{
	verify("parameterDescriptions_md.json", md::MachineModel::Machinedrum,
		26, md::automation::machinedrum::TrackCount, 416);
	verify("parameterDescriptions_mm.json", md::MachineModel::Monomachine,
		58, md::automation::monomachine::TrackCount, 348);
	std::cout << "mdAutomationParameterTest: PASS\n";
	return 0;
}
