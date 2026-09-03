#include "mdautomation.h"

namespace md::automation
{
	namespace
	{
		constexpr uint8_t g_controlChange = 0xb0;

		std::optional<uint8_t> mdController(const ParameterChange& _change)
		{
			if(_change.track >= machinedrum::TrackCount || _change.value > 127)
				return std::nullopt;

			const auto lane = static_cast<uint8_t>(_change.track & 3);
			if(_change.page <= machinedrum::Routing && _change.index < 8)
			{
				constexpr std::array<uint8_t, 4> bases{16, 40, 72, 96};
				return static_cast<uint8_t>(bases[lane] + _change.page * 8
					+ _change.index);
			}
			if(_change.page == machinedrum::Level && _change.index == 0)
				return static_cast<uint8_t>(8 + lane);
			if(_change.page == machinedrum::Mute && _change.index == 0)
				return static_cast<uint8_t>(12 + lane);
			return std::nullopt;
		}

		std::optional<uint8_t> mmController(const ParameterChange& _change)
		{
			if(_change.track >= monomachine::TrackCount || _change.value > 127)
				return std::nullopt;

			constexpr std::array<uint8_t, 7> pageBases{48, 56, 72, 80, 88, 104, 112};
			if(_change.page < pageBases.size() && _change.index < 8)
				return static_cast<uint8_t>(pageBases[_change.page] + _change.index);
			if(_change.page == monomachine::Level && _change.index == 0)
				return 7;
			if(_change.page == monomachine::Mute && _change.index == 0)
				return 3;
			return std::nullopt;
		}
	}

	std::optional<ControlChange> encodeParameterChange(const MachineModel _model,
		const ParameterChange& _change, const uint8_t _baseChannel)
	{
		const auto channelOffset = _model == MachineModel::Monomachine
			? _change.track : static_cast<uint8_t>(_change.track >> 2);
		if(static_cast<unsigned>(_baseChannel) + channelOffset > 15)
			return std::nullopt;

		const auto cc = _model == MachineModel::Monomachine
			? mmController(_change) : mdController(_change);
		if(!cc)
			return std::nullopt;

		return ControlChange{
			static_cast<uint8_t>(g_controlChange | (_baseChannel + channelOffset)),
			*cc,
			_change.value
		};
	}

	std::optional<ParameterChange> decodeParameterChange(const MachineModel _model,
		const ControlChange& _message, const uint8_t _baseChannel)
	{
		if((_message[0] & 0xf0) != g_controlChange || _message[1] > 127
			|| _message[2] > 127)
			return std::nullopt;

		const auto channel = static_cast<uint8_t>(_message[0] & 0x0f);
		if(channel < _baseChannel)
			return std::nullopt;
		const auto channelOffset = static_cast<uint8_t>(channel - _baseChannel);

		ParameterChange result;
		result.value = _message[2];
		const auto cc = _message[1];

		if(_model == MachineModel::Machinedrum)
		{
			if(channelOffset >= 4)
				return std::nullopt;

			uint8_t lane = 0;
			if(cc >= 8 && cc <= 11)
			{
				result.page = machinedrum::Level;
				lane = static_cast<uint8_t>(cc - 8);
			}
			else if(cc >= 12 && cc <= 15)
			{
				result.page = machinedrum::Mute;
				result.value = result.value > 0 ? 1 : 0;
				lane = static_cast<uint8_t>(cc - 12);
			}
			else
			{
				constexpr std::array<uint8_t, 4> bases{16, 40, 72, 96};
				bool found = false;
				for(uint8_t candidate = 0; candidate < bases.size(); ++candidate)
				{
					if(cc < bases[candidate] || cc >= bases[candidate] + 24)
						continue;
					const auto offset = static_cast<uint8_t>(cc - bases[candidate]);
					result.page = static_cast<uint8_t>(offset / 8);
					result.index = static_cast<uint8_t>(offset & 7);
					lane = candidate;
					found = true;
					break;
				}
				if(!found)
					return std::nullopt;
			}
			result.track = static_cast<uint8_t>(channelOffset * 4 + lane);
			return result;
		}

		if(channelOffset >= monomachine::TrackCount)
			return std::nullopt;
		result.track = channelOffset;
		if(cc == 7)
			result.page = monomachine::Level;
		else if(cc == 3)
		{
			result.page = monomachine::Mute;
			result.value = result.value > 0 ? 1 : 0;
		}
		else
		{
			constexpr std::array<uint8_t, 7> pageBases{48, 56, 72, 80, 88, 104, 112};
			bool found = false;
			for(uint8_t page = 0; page < pageBases.size(); ++page)
			{
				if(cc < pageBases[page] || cc >= pageBases[page] + 8)
					continue;
				result.page = page;
				result.index = static_cast<uint8_t>(cc - pageBases[page]);
				found = true;
				break;
			}
			if(!found)
				return std::nullopt;
		}
		return result;
	}
}
