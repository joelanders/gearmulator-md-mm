#include "mdosimage.h"

namespace md
{
	namespace
	{
		constexpr uint8_t g_sysexBegin = 0xf0;
		constexpr uint8_t g_sysexEnd = 0xf7;
		constexpr uint8_t g_eldmanId0 = 0x00;
		constexpr uint8_t g_eldmanId1 = 0x20;
		constexpr uint8_t g_eldmanId2 = 0x3c;
		constexpr uint8_t g_productMachinedrum = 0x02;
		constexpr uint8_t g_productMonomachine = 0x03;
		constexpr uint8_t g_commandData = 0x7e;
		constexpr uint8_t g_commandVerify = 0x7f;
		constexpr size_t g_headerSize = 7;	// F0 + 3 id + product + 00 + command
		constexpr size_t g_blockHeaderSize = 8;	// cs1 cs2 00 a3 a2 a1 a0 00
		constexpr size_t g_groupSize = 3;
		constexpr size_t g_minDataBodySize = g_blockHeaderSize + g_groupSize;
	}

	bool decodeOsUpgradeSysex(OsUpgradeDecodeResult& _result,
		const uint8_t* const _data, const size_t _size)
	{
		OsUpgradeDecodeResult result;
		bool modelSeen = false;
		uint32_t cursor = 0;
		// Upgrades always start past the bootloader, which the running
		// firmware never overwrites through this path.
		constexpr uint32_t g_firstDataAddress = 0x4000;

		size_t pos = 0;
		while(pos < _size)
		{
			if(_data[pos] != g_sysexBegin)
			{
				++pos;	// tolerate stray bytes outside messages (.syx hygiene)
				continue;
			}
			size_t end = pos + 1;
			while(end < _size && _data[end] != g_sysexEnd)
				++end;
			if(end >= _size)
				return false;	// truncated message
			const size_t length = end - pos + 1;
			const uint8_t* const msg = _data + pos;
			pos = end + 1;

			if(length < g_headerSize + 1
				|| msg[1] != g_eldmanId0 || msg[2] != g_eldmanId1
				|| msg[3] != g_eldmanId2 || msg[5] != 0x00)
				return false;
			if(msg[4] != g_productMachinedrum && msg[4] != g_productMonomachine)
				return false;
			const MachineModel model = msg[4] == g_productMachinedrum
				? MachineModel::Machinedrum : MachineModel::Monomachine;
			if(!modelSeen)
			{
				result.model = model;
				modelSeen = true;
			}
			else if(model != result.model)
			{
				return false;	// mixed-model file
			}

			const uint8_t command = msg[6];
			if(command == g_commandVerify)
			{
				result.hasVerifyCommand = true;
				++result.framingMessageCount;
				continue;
			}
			if(command != g_commandData)
				return false;
			const size_t bodySize = length - g_headerSize - 1;
			if(bodySize < g_minDataBodySize)
			{
				// Empty message: framing, never data.
				++result.framingMessageCount;
				continue;
			}
			const uint8_t* const head = msg + g_headerSize;
			const size_t payloadSize = bodySize - g_blockHeaderSize;
			bool wellformed = payloadSize % g_groupSize == 0
				&& head[2] == 0x00 && head[7] == 0x00;
			uint32_t address = 0;
			for(size_t i = 0; wellformed && i < 8; ++i)
			{
				if(i != 2 && i != 7 && head[i] > 0x0f)
					wellformed = false;
			}
			if(wellformed)
			{
				address = (static_cast<uint32_t>(head[3]) << 12)
					| (static_cast<uint32_t>(head[4]) << 8)
					| (static_cast<uint32_t>(head[5]) << 4)
					| static_cast<uint32_t>(head[6]);
				address <<= 4;
			}
			if(!wellformed)
				return false;
			if(result.messages.empty())
			{
				if(address != g_firstDataAddress)
					return false;
			}
			else if(address < cursor)
			{
				// Overlaps already-placed bytes: framing, never a second
				// copy.
				++result.framingMessageCount;
				continue;
			}
			else if(address != cursor)
			{
				return false;	// gap: missing data, refuse, never misplace
			}

			OsUpgradeMessage out;
			out.address = address;
			if(result.messages.empty())
				result.baseAddress = address;
			const uint8_t* const groups = head + g_blockHeaderSize;
			const size_t groupCount = payloadSize / g_groupSize;
			out.data.reserve(groupCount * 2);
			for(size_t g = 0; g < groupCount; ++g)
			{
				const uint32_t collector = groups[g * 3];
				const uint32_t dataHi = groups[g * 3 + 1];
				const uint32_t dataLo = groups[g * 3 + 2];
				if(collector > 0x03 || dataHi > 0x7f || dataLo > 0x7f)
					return false;
				const uint32_t word = dataLo | (dataHi << 7) | (collector << 14);
				out.data.push_back(static_cast<uint8_t>(word >> 8));
				out.data.push_back(static_cast<uint8_t>(word & 0xff));
			}
			cursor = address + static_cast<uint32_t>(out.data.size());
			if(cursor > g_romSize)
				return false;
			result.messages.push_back(std::move(out));
			++result.dataMessageCount;
		}

		if(result.messages.empty())
			return false;
		result.endAddress = cursor;
		_result = std::move(result);
		return true;
	}
}
