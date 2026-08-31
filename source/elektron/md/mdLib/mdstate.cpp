#include "mdstate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace md
{
	namespace
	{
		constexpr std::array<uint8_t, 4> g_magic = {'M', 'D', 'S', 'T'};
		constexpr uint16_t g_version1 = 1;
		constexpr uint16_t g_version1HeaderSize = 20;
		constexpr uint16_t g_versionWithUserFlash = 2;
		constexpr uint16_t g_versionWithUserFlashHeaderSize = 28;
		constexpr uint16_t g_version3 = 3;
		constexpr uint16_t g_version3HeaderSize = 44;
		constexpr uint16_t g_version3FlashFlag = 1;
		constexpr uint32_t g_sectorEntryHeaderSize = 8;
		constexpr std::array<uint8_t, 4> g_cacheMagic = {'M', 'D', 'F', 'C'};
		constexpr uint16_t g_cacheVersion = 1;
		constexpr uint16_t g_cacheHeaderSize = 28;

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

		void appendU64(std::vector<uint8_t>& _dst, const uint64_t _value)
		{
			appendU32(_dst, static_cast<uint32_t>(_value >> 32));
			appendU32(_dst, static_cast<uint32_t>(_value));
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

		uint64_t readU64(const std::vector<uint8_t>& _src, const size_t _offset)
		{
			return (static_cast<uint64_t>(readU32(_src, _offset)) << 32)
				| readU32(_src, _offset + 4);
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

		uint64_t fingerprint(const std::vector<uint8_t>& _data)
		{
			uint64_t result = 14695981039346656037ull;
			for(const auto byte : _data)
			{
				result ^= byte;
				result *= 1099511628211ull;
			}
			return result;
		}

		bool validStateType(const synthLib::StateType _type)
		{
			return _type == synthLib::StateTypeGlobal ||
				_type == synthLib::StateTypeCurrentProgram;
		}

		bool hasMagic(const std::vector<uint8_t>& _state)
		{
			return _state.size() >= g_magic.size()
				&& std::equal(g_magic.begin(), g_magic.end(), _state.begin());
		}

		bool decodeVersion1(std::vector<uint8_t>& _patchRam,
			const std::vector<uint8_t>& _state, const MachineModel _expectedModel,
			const synthLib::StateType _expectedType)
		{
			if(_state.size() < g_version1HeaderSize || !hasMagic(_state)
				|| readU16(_state, 4) != g_version1
				|| readU16(_state, 6) != g_version1HeaderSize)
				return false;
			if(_state[8] != modelTag(_expectedModel)
				|| _state[9] != static_cast<uint8_t>(_expectedType)
				|| readU16(_state, 10) != 0)
				return false;

			const auto payloadSize = readU32(_state, 12);
			if(payloadSize != g_patchRamStateSize
				|| _state.size() != static_cast<size_t>(g_version1HeaderSize) + payloadSize)
				return false;
			const auto* const payload = _state.data() + g_version1HeaderSize;
			if(readU32(_state, 16) != crc32(payload, payloadSize))
				return false;

			std::vector<uint8_t> decoded(payload, payload + payloadSize);
			_patchRam.swap(decoded);
			return true;
		}
	}

	bool encodeState(std::vector<uint8_t>& _state, const std::vector<uint8_t>& _patchRam,
		const MachineModel _model, const synthLib::StateType _type,
		const std::vector<uint8_t>& _userFlash)
	{
		if(_patchRam.size() != g_patchRamStateSize || !validStateType(_type))
			return false;
		const bool hasUserFlash = !_userFlash.empty();
		if(hasUserFlash && (_model != MachineModel::Monomachine
			|| _userFlash.size() != g_mmUserFlashStateSize))
			return false;

		const auto originalSize = _state.size();
		const auto headerSize = hasUserFlash
			? g_versionWithUserFlashHeaderSize : g_version1HeaderSize;
		_state.reserve(originalSize + headerSize + _patchRam.size() + _userFlash.size());
		_state.insert(_state.end(), g_magic.begin(), g_magic.end());
		appendU16(_state, hasUserFlash ? g_versionWithUserFlash : g_version1);
		appendU16(_state, headerSize);
		_state.push_back(modelTag(_model));
		_state.push_back(static_cast<uint8_t>(_type));
		appendU16(_state, 0);
		appendU32(_state, static_cast<uint32_t>(_patchRam.size()));
		appendU32(_state, crc32(_patchRam.data(), _patchRam.size()));
		if(hasUserFlash)
		{
			appendU32(_state, static_cast<uint32_t>(_userFlash.size()));
			appendU32(_state, crc32(_userFlash.data(), _userFlash.size()));
		}
		_state.insert(_state.end(), _patchRam.begin(), _patchRam.end());
		_state.insert(_state.end(), _userFlash.begin(), _userFlash.end());
		return true;
	}

	bool encodeState(std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _patchRam,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _flashBaseline,
		const MachineModel _model, const synthLib::StateType _type)
	{
		if(_model != MachineModel::Machinedrum
			|| _patchRam.size() != g_patchRamStateSize || !validStateType(_type)
			|| _flashData.size() != _flashBaseline.size() || _flashData.empty()
			|| (_flashData.size() % g_uwFlashSectorSize) != 0)
			return false;

		const auto totalSectors = _flashData.size() / g_uwFlashSectorSize;
		if(totalSectors > std::numeric_limits<uint16_t>::max())
			return false;
		std::vector<uint16_t> changedSectors;
		changedSectors.reserve(totalSectors);
		for(size_t sector = 0; sector < totalSectors; ++sector)
		{
			const auto offset = sector * g_uwFlashSectorSize;
			if(!std::equal(_flashData.begin() + offset,
				_flashData.begin() + offset + g_uwFlashSectorSize,
				_flashBaseline.begin() + offset))
				changedSectors.push_back(static_cast<uint16_t>(sector));
		}

		const auto originalSize = _state.size();
		const auto entrySize = static_cast<size_t>(g_sectorEntryHeaderSize)
			+ g_uwFlashSectorSize;
		_state.reserve(originalSize + g_version3HeaderSize + _patchRam.size()
			+ changedSectors.size() * entrySize);
		_state.insert(_state.end(), g_magic.begin(), g_magic.end());
		appendU16(_state, g_version3);
		appendU16(_state, g_version3HeaderSize);
		_state.push_back(modelTag(_model));
		_state.push_back(static_cast<uint8_t>(_type));
		appendU16(_state, g_version3FlashFlag);
		appendU32(_state, static_cast<uint32_t>(_patchRam.size()));
		appendU32(_state, crc32(_patchRam.data(), _patchRam.size()));
		appendU64(_state, fingerprint(_flashBaseline));
		appendU32(_state, static_cast<uint32_t>(_flashData.size()));
		appendU32(_state, g_uwFlashSectorSize);
		appendU16(_state, static_cast<uint16_t>(changedSectors.size()));
		appendU16(_state, 0);
		const auto overlayCrcOffset = _state.size();
		appendU32(_state, 0);
		_state.insert(_state.end(), _patchRam.begin(), _patchRam.end());
		const auto overlayOffset = _state.size();
		for(const auto sector : changedSectors)
		{
			const auto offset = static_cast<size_t>(sector) * g_uwFlashSectorSize;
			appendU16(_state, sector);
			appendU16(_state, 0);
			appendU32(_state, crc32(_flashData.data() + offset, g_uwFlashSectorSize));
			_state.insert(_state.end(), _flashData.begin() + offset,
				_flashData.begin() + offset + g_uwFlashSectorSize);
		}
		const auto overlayCrc = crc32(_state.data() + overlayOffset,
			_state.size() - overlayOffset);
		_state[overlayCrcOffset] = static_cast<uint8_t>(overlayCrc >> 24);
		_state[overlayCrcOffset + 1] = static_cast<uint8_t>(overlayCrc >> 16);
		_state[overlayCrcOffset + 2] = static_cast<uint8_t>(overlayCrc >> 8);
		_state[overlayCrcOffset + 3] = static_cast<uint8_t>(overlayCrc);
		return true;
	}

	bool decodeState(std::vector<uint8_t>& _patchRam, const std::vector<uint8_t>& _state,
		const MachineModel _expectedModel, const synthLib::StateType _expectedType)
	{
		std::vector<uint8_t> ignoredUserFlash;
		return decodeState(_patchRam, ignoredUserFlash, _state, _expectedModel,
			_expectedType);
	}

	bool decodeState(std::vector<uint8_t>& _patchRam, std::vector<uint8_t>& _userFlash,
		const std::vector<uint8_t>& _state, const MachineModel _expectedModel,
		const synthLib::StateType _expectedType)
	{
		if(!validStateType(_expectedType) || _state.size() < g_version1HeaderSize
			|| !hasMagic(_state))
			return false;

		const auto version = readU16(_state, 4);
		const auto headerSize = readU16(_state, 6);
		if((version == g_version1 && headerSize != g_version1HeaderSize)
			|| (version == g_versionWithUserFlash
				&& headerSize != g_versionWithUserFlashHeaderSize)
			|| (version != g_version1 && version != g_versionWithUserFlash))
			return false;
		if(_state[8] != modelTag(_expectedModel)
			|| _state[9] != static_cast<uint8_t>(_expectedType)
			|| readU16(_state, 10) != 0)
			return false;

		const auto patchSize = readU32(_state, 12);
		if(patchSize != g_patchRamStateSize)
			return false;
		uint32_t userFlashSize = 0;
		if(version == g_versionWithUserFlash)
		{
			userFlashSize = readU32(_state, 20);
			if(_expectedModel != MachineModel::Monomachine
				|| userFlashSize != g_mmUserFlashStateSize)
				return false;
		}
		if(_state.size() != static_cast<size_t>(headerSize) + patchSize + userFlashSize)
			return false;

		const auto* const patch = _state.data() + headerSize;
		if(readU32(_state, 16) != crc32(patch, patchSize))
			return false;
		const auto* const userFlash = patch + patchSize;
		if(userFlashSize && readU32(_state, 24) != crc32(userFlash, userFlashSize))
			return false;

		std::vector<uint8_t> decodedPatch(patch, patch + patchSize);
		std::vector<uint8_t> decodedUserFlash(userFlash, userFlash + userFlashSize);
		_patchRam.swap(decodedPatch);
		_userFlash.swap(decodedUserFlash);
		return true;
	}

	bool decodeState(DecodedState& _decoded, const std::vector<uint8_t>& _state,
		const std::vector<uint8_t>& _flashBaseline,
		const MachineModel _expectedModel, const synthLib::StateType _expectedType)
	{
		if(!validStateType(_expectedType) || _state.size() < 8 || !hasMagic(_state))
			return false;
		if(readU16(_state, 4) == g_version1)
		{
			DecodedState decoded;
			if(!decodeVersion1(decoded.patchRam, _state, _expectedModel, _expectedType))
				return false;
			_decoded = std::move(decoded);
			return true;
		}
		if(readU16(_state, 4) != g_version3 || _state.size() < g_version3HeaderSize
			|| readU16(_state, 6) != g_version3HeaderSize
			|| _expectedModel != MachineModel::Machinedrum
			|| _state[8] != modelTag(_expectedModel)
			|| _state[9] != static_cast<uint8_t>(_expectedType)
			|| readU16(_state, 10) != g_version3FlashFlag)
			return false;

		const auto patchSize = readU32(_state, 12);
		const auto flashSize = readU32(_state, 28);
		const auto sectorSize = readU32(_state, 32);
		const auto sectorCount = readU16(_state, 36);
		if(patchSize != g_patchRamStateSize || readU16(_state, 38) != 0
			|| flashSize != _flashBaseline.size() || flashSize == 0
			|| sectorSize != g_uwFlashSectorSize
			|| (flashSize % sectorSize) != 0
			|| sectorCount > flashSize / sectorSize
			|| readU64(_state, 20) != fingerprint(_flashBaseline))
			return false;

		const auto entrySize = static_cast<size_t>(g_sectorEntryHeaderSize) + sectorSize;
		const auto expectedSize = static_cast<size_t>(g_version3HeaderSize) + patchSize
			+ static_cast<size_t>(sectorCount) * entrySize;
		if(_state.size() != expectedSize)
			return false;
		const auto* const patch = _state.data() + g_version3HeaderSize;
		if(readU32(_state, 16) != crc32(patch, patchSize))
			return false;
		const auto overlayOffset = static_cast<size_t>(g_version3HeaderSize) + patchSize;
		if(readU32(_state, 40) != crc32(_state.data() + overlayOffset,
			_state.size() - overlayOffset))
			return false;

		DecodedState decoded;
		decoded.patchRam.assign(patch, patch + patchSize);
		decoded.flashData = _flashBaseline;
		decoded.containsFlash = true;
		size_t offset = overlayOffset;
		uint16_t previousSector = 0;
		for(uint16_t entry = 0; entry < sectorCount; ++entry)
		{
			const auto sector = readU16(_state, offset);
			if(readU16(_state, offset + 2) != 0
				|| sector >= flashSize / sectorSize
				|| (entry > 0 && sector <= previousSector))
				return false;
			const auto* const data = _state.data() + offset + g_sectorEntryHeaderSize;
			if(readU32(_state, offset + 4) != crc32(data, sectorSize))
				return false;
			std::copy_n(data, sectorSize,
				decoded.flashData.begin() + static_cast<size_t>(sector) * sectorSize);
			previousSector = sector;
			offset += entrySize;
		}
		_decoded = std::move(decoded);
		return true;
	}

	bool encodeFactoryFlashCache(std::vector<uint8_t>& _cache,
		const std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _romBaseline)
	{
		if(_flashData.size() != g_romSize || _romBaseline.size() != g_romSize)
			return false;
		const auto originalSize = _cache.size();
		_cache.reserve(originalSize + g_cacheHeaderSize + _flashData.size());
		_cache.insert(_cache.end(), g_cacheMagic.begin(), g_cacheMagic.end());
		appendU16(_cache, g_cacheVersion);
		appendU16(_cache, g_cacheHeaderSize);
		appendU64(_cache, fingerprint(_romBaseline));
		appendU32(_cache, static_cast<uint32_t>(_flashData.size()));
		appendU32(_cache, crc32(_flashData.data(), _flashData.size()));
		appendU32(_cache, 0);
		_cache.insert(_cache.end(), _flashData.begin(), _flashData.end());
		return true;
	}

	bool decodeFactoryFlashCache(std::vector<uint8_t>& _flashData,
		const std::vector<uint8_t>& _cache,
		const std::vector<uint8_t>& _romBaseline)
	{
		if(_romBaseline.size() != g_romSize
			|| _cache.size() != static_cast<size_t>(g_cacheHeaderSize) + g_romSize
			|| !std::equal(g_cacheMagic.begin(), g_cacheMagic.end(), _cache.begin())
			|| readU16(_cache, 4) != g_cacheVersion
			|| readU16(_cache, 6) != g_cacheHeaderSize
			|| readU64(_cache, 8) != fingerprint(_romBaseline)
			|| readU32(_cache, 16) != g_romSize || readU32(_cache, 24) != 0)
			return false;
		const auto* const payload = _cache.data() + g_cacheHeaderSize;
		if(readU32(_cache, 20) != crc32(payload, g_romSize))
			return false;
		std::vector<uint8_t> decoded(payload, payload + g_romSize);
		_flashData.swap(decoded);
		return true;
	}
}
