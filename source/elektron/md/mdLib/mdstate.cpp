#include "mdstate.h"

#include <array>
#include <cstddef>

namespace md
{
	namespace
	{
		constexpr std::array<uint8_t, 4> g_magic = {'M', 'D', 'S', 'T'};
		constexpr uint16_t g_version = 1;
		constexpr uint16_t g_headerSize = 20;

		void appendU16(std::vector<uint8_t>& _dst, const uint16_t _value)
		{
			_dst.push_back(static_cast<uint8_t>(_value >> 8));
			_dst.push_back(static_cast<uint8_t>(_value));
		}

		void appendU32(std::vector<uint8_t>& _dst, const uint32_t _value)
		{
			_dst.push_back(static_cast<uint8_t>(_value >> 24));
			_dst.push_back(static_cast<uint8_t>(_value >> 16));
			_dst.push_back(static_cast<uint8_t>(_value >> 8));
			_dst.push_back(static_cast<uint8_t>(_value));
		}

		uint16_t readU16(const std::vector<uint8_t>& _src, const size_t _offset)
		{
			return static_cast<uint16_t>(
				(static_cast<uint16_t>(_src[_offset]) << 8) |
				static_cast<uint16_t>(_src[_offset + 1]));
		}

		uint32_t readU32(const std::vector<uint8_t>& _src, const size_t _offset)
		{
			return
				(static_cast<uint32_t>(_src[_offset]) << 24) |
				(static_cast<uint32_t>(_src[_offset + 1]) << 16) |
				(static_cast<uint32_t>(_src[_offset + 2]) << 8) |
				static_cast<uint32_t>(_src[_offset + 3]);
		}

		uint32_t crc32(const uint8_t* const _data, const size_t _size)
		{
			uint32_t crc = 0xffffffffu;
			for(size_t i = 0; i < _size; ++i)
			{
				crc ^= _data[i];
				for(uint32_t bit = 0; bit < 8; ++bit)
					crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
			}
			return ~crc;
		}

		uint8_t modelTag(const MachineModel _model)
		{
			return _model == MachineModel::Monomachine ? 1 : 0;
		}

		bool validStateType(const synthLib::StateType _type)
		{
			return _type == synthLib::StateTypeGlobal ||
				_type == synthLib::StateTypeCurrentProgram;
		}
	}

	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		const MachineModel _model, const synthLib::StateType _type)
	{
		if(_patchRam.size() != g_patchRamStateSize || !validStateType(_type))
			return false;

		const auto originalSize = _state.size();
		_state.reserve(originalSize + g_headerSize + _patchRam.size());
		_state.insert(_state.end(), g_magic.begin(), g_magic.end());
		appendU16(_state, g_version);
		appendU16(_state, g_headerSize);
		_state.push_back(modelTag(_model));
		_state.push_back(static_cast<uint8_t>(_type));
		appendU16(_state, 0);
		appendU32(_state, static_cast<uint32_t>(_patchRam.size()));
		appendU32(_state, crc32(_patchRam.data(), _patchRam.size()));
		_state.insert(_state.end(), _patchRam.begin(), _patchRam.end());
		return true;
	}

	bool decodeState(std::vector<uint8_t>& _patchRam, const std::vector<uint8_t>& _state,
		const MachineModel _expectedModel, const synthLib::StateType _expectedType)
	{
		if(!validStateType(_expectedType) || _state.size() < g_headerSize)
			return false;
		for(size_t i = 0; i < g_magic.size(); ++i)
			if(_state[i] != g_magic[i])
				return false;
		if(readU16(_state, 4) != g_version || readU16(_state, 6) != g_headerSize)
			return false;
		if(_state[8] != modelTag(_expectedModel) ||
			_state[9] != static_cast<uint8_t>(_expectedType) ||
			readU16(_state, 10) != 0)
			return false;

		const auto payloadSize = readU32(_state, 12);
		if(payloadSize != g_patchRamStateSize ||
			_state.size() != static_cast<size_t>(g_headerSize) + payloadSize)
			return false;

		const auto* const payload = _state.data() + g_headerSize;
		if(readU32(_state, 16) != crc32(payload, payloadSize))
			return false;

		std::vector<uint8_t> decoded(payload, payload + payloadSize);
		_patchRam.swap(decoded);
		return true;
	}
}
