#include "mdMcpRamTools.h"

#if GEARMULATOR_MDMM_RAM_DIAGNOSTICS

#include "mdPluginProcessor.h"

#include "mdLib/mddevice.h"
#include "mdLib/mdRamLabels.h"
#include "mdLib/mdramdiff.h"

#include "mcpServerLib/mcpServer.h"
#include "mcpServerLib/mcpTool.h"

#include "synthLib/device.h"
#include "synthLib/plugin.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace mdJucePlugin
{
namespace
{
	md::ramDiff::RegionKind parseRegion(const std::string& _name)
	{
		if(_name == "main")
			return md::ramDiff::RegionKind::Main;
		if(_name == "sram")
			return md::ramDiff::RegionKind::Sram;
		if(_name == "loader")
			return md::ramDiff::RegionKind::Loader;
		return md::ramDiff::RegionKind::Patch;
	}

	std::string formatHex(const std::vector<uint8_t>& _bytes)
	{
		std::ostringstream out;
		out << std::hex << std::uppercase;
		for(size_t i = 0; i < _bytes.size(); ++i)
		{
			if(i)
				out << ' ';
			out.width(2);
			out.fill('0');
			out << static_cast<unsigned>(_bytes[i]);
		}
		return out.str();
	}

	bool parseHex(const std::string& _text, std::vector<uint8_t>& _bytes)
	{
		_bytes.clear();
		unsigned value = 0;
		int digits = 0;
		const auto flush = [&]()
		{
			if(digits == 0)
				return true;
			if(digits != 2)
				return false;
			_bytes.push_back(static_cast<uint8_t>(value));
			value = 0;
			digits = 0;
			return true;
		};
		for(const auto ch : _text)
		{
			if(std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == ':')
			{
				if(!flush())
					return false;
				continue;
			}
			unsigned digit = 0;
			if(ch >= '0' && ch <= '9')
				digit = static_cast<unsigned>(ch - '0');
			else if(ch >= 'a' && ch <= 'f')
				digit = static_cast<unsigned>(ch - 'a' + 10);
			else if(ch >= 'A' && ch <= 'F')
				digit = static_cast<unsigned>(ch - 'A' + 10);
			else
				return false;
			value = (value << 4) | digit;
			++digits;
			if(digits == 2 && !flush())
				return false;
		}
		return flush();
	}

	std::string formatAddress(const uint32_t _address)
	{
		char text[11];
		std::snprintf(text, sizeof(text), "0x%08X", _address);
		return text;
	}

	mcpServer::JsonValue segmentJson(const md::ramLabels::Segment& _segment)
	{
		auto json = mcpServer::JsonValue::object();
		json.set("name", mcpServer::JsonValue::fromString(_segment.name));
		json.set("label", mcpServer::JsonValue::fromString(_segment.label));
		json.set("region", mcpServer::JsonValue::fromString(md::ramDiff::regionName(_segment.region)));
		json.set("address", mcpServer::JsonValue::fromString(formatAddress(_segment.address)));
		json.set("size", mcpServer::JsonValue::fromInt(static_cast<int>(_segment.size)));
		return json;
	}

	uint32_t parseAddress(const mcpServer::JsonValue& _value)
	{
		if(_value.isInt())
			return static_cast<uint32_t>(_value.getInt());
		return static_cast<uint32_t>(std::stoul(_value.getString().toStdString(), nullptr, 0));
	}

	bool resolveRequest(const mcpServer::JsonValue& _params, const bool _namedMap,
		const bool _needSize, md::ramDiff::RegionKind& _kind, uint32_t& _address, size_t& _size,
		md::ramLabels::Segment& _segment)
	{
		if(_params.isObject() && _params.hasProperty("name"))
		{
			if(!_namedMap)
				throw std::runtime_error("named RAM segments are only available on Machinedrum");
			const auto name = _params.get("name").getString().toStdString();
			auto found = md::ramLabels::resolveSegment(md::ramLabels::probedMap(), name);
			if(!found)
				throw std::runtime_error("unknown RAM segment: " + name);
			_segment = *found;
			_kind = found->region;
			_address = found->address;
			_size = found->size;
			return true;
		}

		if(!_params.isObject() || !_params.hasProperty("region")
			|| !_params.hasProperty("address")
			|| (_needSize && !_params.hasProperty("size")))
			throw std::runtime_error("provide name, or region + address + size");

		_kind = parseRegion(_params.get("region").getString().toStdString());
		_address = parseAddress(_params.get("address"));
		if(_needSize)
			_size = static_cast<size_t>(_params.get("size").getInt());
		_segment = {};
		_segment.region = _kind;
		_segment.address = _address;
		_segment.size = static_cast<uint32_t>(_size);
		_segment.name = md::ramDiff::regionName(_kind);
		_segment.label = _segment.name;
		return true;
	}

	md::Hardware* lockedHardware(synthLib::Device* const _device)
	{
		auto* const device = dynamic_cast<md::Device*>(_device);
		return device ? &device->getHardware() : nullptr;
	}
}

void registerMdRamTools(mcpServer::McpServer& _server, AudioPluginAudioProcessor& _processor)
{
	const bool namedMap = _processor.getModel() == md::MachineModel::Machinedrum;

	{
		mcpServer::ToolDef tool;
		tool.name = "ram_map";
		tool.description = namedMap
			? "List Machinedrum named RAM segments (patterns A01-H16, current kit, songs, globals)."
			: "List Monomachine writable RAM regions. Named pattern/kit slots are Machinedrum-only; use region+address+size.";
		tool.inputSchema.addEnumProperty("record", "Optional record filter",
			{"current", "pattern", "kit", "song", "global"}, false);
		tool.handler = [namedMap](const mcpServer::JsonValue& _params) -> mcpServer::JsonValue
		{
			auto result = mcpServer::JsonValue::array();
			if(!namedMap)
			{
				const md::ramDiff::RegionKind kinds[] = {
					md::ramDiff::RegionKind::Patch, md::ramDiff::RegionKind::Main,
					md::ramDiff::RegionKind::Sram, md::ramDiff::RegionKind::Loader
				};
				for(const auto kind : kinds)
				{
					const auto range = md::ramDiff::regionRange(kind);
					md::ramLabels::Segment segment;
					segment.region = kind;
					segment.address = range.begin;
					segment.size = range.size();
					segment.name = md::ramDiff::regionName(kind);
					segment.label = std::string(md::ramDiff::regionName(kind)) + " (use ram_read with region+address+size, max "
						+ std::to_string(md::ramDiff::g_rangeIoMaxBytes) + " bytes)";
					result.append(segmentJson(segment));
				}
				return result;
			}
			const auto filter = _params.isObject() && _params.hasProperty("record")
				? _params.get("record").getString().toStdString() : std::string();
			for(const auto& segment : md::ramLabels::listSegments(md::ramLabels::probedMap(), filter))
				result.append(segmentJson(segment));
			return result;
		};
		_server.registerTool(std::move(tool));
	}

	{
		mcpServer::ToolDef tool;
		tool.name = "ram_read";
		tool.description = "Read a RAM segment. Machinedrum: pass name (e.g. pattern:A01:trigs:1). Both products: region+address+size. Max 8192 bytes.";
		tool.inputSchema.addProperty("name", "string", "Named segment (Machinedrum)", false);
		tool.inputSchema.addEnumProperty("region", "RAM region",
			{"patch", "main", "sram", "loader"}, false);
		tool.inputSchema.addProperty("address", "string", "Start address (hex, e.g. 0x001272c0)", false);
		tool.inputSchema.addIntProperty("size", "Byte count", false, 1,
			static_cast<int>(md::ramDiff::g_rangeIoMaxBytes));
		tool.handler = [&_processor, namedMap](const mcpServer::JsonValue& _params) -> mcpServer::JsonValue
		{
			md::ramDiff::RegionKind kind = md::ramDiff::RegionKind::Patch;
			uint32_t address = 0;
			size_t size = 0;
			md::ramLabels::Segment segment;
			resolveRequest(_params, namedMap, true, kind, address, size, segment);

			std::vector<uint8_t> bytes;
			const auto ok = _processor.getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* hardware = lockedHardware(_device);
					return hardware && hardware->copyWorkingRamRange(kind, address, size, bytes);
				});
			if(!ok)
				throw std::runtime_error("RAM read failed (range invalid or device busy)");

			auto result = segmentJson(segment);
			result.set("hex", mcpServer::JsonValue::fromString(formatHex(bytes)));
			return result;
		};
		_server.registerTool(std::move(tool));
	}

	{
		mcpServer::ToolDef tool;
		tool.name = "ram_write";
		tool.description = "Write a RAM segment. Machinedrum: pass name. Both products: region+address+hex. Optional verify reads back.";
		tool.inputSchema.addProperty("name", "string", "Named segment (Machinedrum)", false);
		tool.inputSchema.addEnumProperty("region", "RAM region",
			{"patch", "main", "sram", "loader"}, false);
		tool.inputSchema.addProperty("address", "string", "Start address (hex)", false);
		tool.inputSchema.addProperty("hex", "string", "Bytes as hex (e.g. '00 00 00 55')", true);
		tool.inputSchema.addProperty("verify", "boolean", "Read back after write", false);
		tool.handler = [&_processor, namedMap](const mcpServer::JsonValue& _params) -> mcpServer::JsonValue
		{
			std::vector<uint8_t> payload;
			if(!parseHex(_params.get("hex").getString().toStdString(), payload) || payload.empty())
				throw std::runtime_error("hex must be a non-empty even-length hex string");

			md::ramDiff::RegionKind kind = md::ramDiff::RegionKind::Patch;
			uint32_t address = 0;
			size_t size = 0;
			md::ramLabels::Segment segment;
			resolveRequest(_params, namedMap, false, kind, address, size, segment);
			if(_params.hasProperty("name") && payload.size() != size)
				throw std::runtime_error("hex length must match the named segment");
			if(!_params.hasProperty("name"))
			{
				size = payload.size();
				segment.size = static_cast<uint32_t>(size);
			}

			std::vector<uint8_t> verify;
			const bool doVerify = _params.isObject() && _params.hasProperty("verify")
				&& _params.get("verify").getBool();
			const auto ok = _processor.getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* hardware = lockedHardware(_device);
					if(!hardware)
						return false;
					if(!hardware->writeWorkingRamRange(kind, address, payload.data(), payload.size()))
						return false;
					if(doVerify)
						return hardware->copyWorkingRamRange(kind, address, payload.size(), verify);
					return true;
				});
			if(!ok)
				throw std::runtime_error("RAM write failed (range invalid, restore in progress, or device busy)");

			auto result = segmentJson(segment);
			result.set("written", mcpServer::JsonValue::fromInt(static_cast<int>(payload.size())));
			if(doVerify)
				result.set("hex", mcpServer::JsonValue::fromString(formatHex(verify)));
			return result;
		};
		_server.registerTool(std::move(tool));
	}

	{
		mcpServer::ToolDef tool;
		tool.name = "ram_fill";
		tool.description = "Fill a RAM segment with one byte (e.g. clear a trig page).";
		tool.inputSchema.addProperty("name", "string", "Named segment (Machinedrum)", false);
		tool.inputSchema.addEnumProperty("region", "RAM region",
			{"patch", "main", "sram", "loader"}, false);
		tool.inputSchema.addProperty("address", "string", "Start address (hex)", false);
		tool.inputSchema.addIntProperty("size", "Byte count when not using name", false, 1,
			static_cast<int>(md::ramDiff::g_rangeIoMaxBytes));
		tool.inputSchema.addIntProperty("byte", "Fill value 0-255", true, 0, 255);
		tool.handler = [&_processor, namedMap](const mcpServer::JsonValue& _params) -> mcpServer::JsonValue
		{
			md::ramDiff::RegionKind kind = md::ramDiff::RegionKind::Patch;
			uint32_t address = 0;
			size_t size = 0;
			md::ramLabels::Segment segment;
			resolveRequest(_params, namedMap, true, kind, address, size, segment);
			const auto fill = static_cast<uint8_t>(_params.get("byte").getInt());
			std::vector<uint8_t> payload(size, fill);
			const auto ok = _processor.getPlugin().withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* hardware = lockedHardware(_device);
					return hardware && hardware->writeWorkingRamRange(
						kind, address, payload.data(), payload.size());
				});
			if(!ok)
				throw std::runtime_error("RAM fill failed");
			auto result = segmentJson(segment);
			result.set("written", mcpServer::JsonValue::fromInt(static_cast<int>(payload.size())));
			return result;
		};
		_server.registerTool(std::move(tool));
	}
}
}

#endif