#include "mdLib/mdplusdrive.h"
#include "mdLib/mdstate.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

namespace
{
	constexpr size_t HeaderSize = 16;
	constexpr size_t RecordSize = 4 + 512;

	void writeBe32(std::vector<uint8_t>& _data, const size_t _offset,
		const uint32_t _value)
	{
		_data[_offset] = static_cast<uint8_t>(_value >> 24);
		_data[_offset + 1] = static_cast<uint8_t>(_value >> 16);
		_data[_offset + 2] = static_cast<uint8_t>(_value >> 8);
		_data[_offset + 3] = static_cast<uint8_t>(_value);
	}

	std::vector<uint8_t> makeImage(const uint32_t _sectorCount)
	{
		std::vector<uint8_t> image(HeaderSize
			+ static_cast<size_t>(_sectorCount) * RecordSize);
		image[0] = 'M';
		image[1] = 'D';
		image[2] = 'P';
		image[3] = 'D';
		writeBe32(image, 4, 1);
		writeBe32(image, 8, 512);
		writeBe32(image, 12, _sectorCount);
		for(uint32_t sector = 0; sector < _sectorCount; ++sector)
		{
			const auto offset = HeaderSize + static_cast<size_t>(sector) * RecordSize;
			writeBe32(image, offset, sector);
			image[offset + 4] = static_cast<uint8_t>(sector);
			image[offset + RecordSize - 1] = static_cast<uint8_t>(sector >> 8);
		}
		return image;
	}

	long long millisecondsSince(const std::chrono::steady_clock::time_point _start)
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _start).count();
	}

	uint64_t peakResidentBytes()
	{
#if defined(__APPLE__) || defined(__linux__)
		rusage usage{};
		if(getrusage(RUSAGE_SELF, &usage) != 0)
			return 0;
#if defined(__APPLE__)
		return static_cast<uint64_t>(usage.ru_maxrss);
#else
		return static_cast<uint64_t>(usage.ru_maxrss) * 1024u;
#endif
#else
		return 0;
#endif
	}
}

int main(const int _argc, const char* const* _argv)
{
	try
	{
		const auto maximumRecords = static_cast<uint32_t>(
			(md::g_plusDriveMaxSerializedBytes - HeaderSize) / RecordSize);
		const auto records = _argc > 1
			? static_cast<uint64_t>(std::stoull(_argv[1]))
			: static_cast<uint64_t>(maximumRecords);
		if(records > maximumRecords
			|| records > std::numeric_limits<uint32_t>::max())
		{
			std::cerr << "sector count exceeds the bounded MDPD state maximum\n";
			return 2;
		}

		const auto buildStart = std::chrono::steady_clock::now();
		auto plusDrive = makeImage(static_cast<uint32_t>(records));
		const auto buildMs = millisecondsSince(buildStart);
		if(!md::PlusDrive::validateStorage(plusDrive))
		{
			std::cerr << "generated MDPD image was invalid\n";
			return 1;
		}

		std::vector<uint8_t> patchRam(md::g_patchRamStateSize);
		std::vector<uint8_t> rom(md::g_romSize);
		auto factoryFlash = rom;
		factoryFlash[0] = 1;
		const auto flash = factoryFlash;

		const auto encodeStart = std::chrono::steady_clock::now();
		std::vector<uint8_t> projectState;
		if(!md::encodeState(projectState, patchRam, flash, factoryFlash, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal, plusDrive))
		{
			std::cerr << "project-state encode failed\n";
			return 1;
		}
		const auto encodeMs = millisecondsSince(encodeStart);

		const auto decodeStart = std::chrono::steady_clock::now();
		md::DecodedState decoded;
		if(!md::decodeState(decoded, projectState, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal)
			|| !decoded.containsPlusDrive || decoded.plusDrive != plusDrive)
		{
			std::cerr << "project-state round trip failed\n";
			return 1;
		}
		const auto decodeMs = millisecondsSince(decodeStart);

		std::cout << "records=" << records
			<< " image_bytes=" << plusDrive.size()
			<< " project_state_bytes=" << projectState.size()
			<< " build_ms=" << buildMs
			<< " encode_ms=" << encodeMs
			<< " decode_ms=" << decodeMs
			<< " peak_resident_bytes=" << peakResidentBytes() << '\n';
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << "mdPlusDriveStateWorkload: " << error.what() << '\n';
		return 2;
	}
}
