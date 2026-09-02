#include "mdLib/mdmmwaveforms.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdtypes.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr uint32_t g_canonicalRomSize = 0x800000;
	constexpr uint64_t g_canonicalMd163Fnv1a = 0x33b7c1a9e29f43fdull;
	constexpr uint64_t g_canonicalMm132bFnv1a = 0xe1c1b461b6d0f21bull;
	constexpr uint32_t g_canonicalSourceBase = 0x100000;
	constexpr uint32_t g_canonicalRecordBytes = 0x17fc;
	constexpr uint32_t g_canonicalHeaderBytes = 8;
	constexpr uint32_t g_canonicalWaveformCount = 64;
	constexpr uint32_t g_canonicalPayloadWords = 2044;
	constexpr uint32_t g_canonicalDestinationBase = 0x150000;
	constexpr uint32_t g_canonicalTransferWords = 0x800;

	bool check(const bool _condition, const std::string& _message)
	{
		if(_condition)
			return true;
		std::cerr << "FAIL: " << _message << '\n';
		return false;
	}

	uint32_t waveformWord(const uint32_t _wave, const uint32_t _word)
	{
		return ((_wave + 1u) << 17u) ^ (_word * 0x1021u) ^ 0x0055aau;
	}

	std::vector<uint8_t> makeFactoryImage()
	{
		std::vector<uint8_t> image(g_canonicalRomSize, 0xff);
		for(uint32_t wave = 0; wave < g_canonicalWaveformCount; ++wave)
		{
			const auto record = g_canonicalSourceBase + wave * g_canonicalRecordBytes;
			image[record] = 0x1a;
			image[record + 1] = static_cast<uint8_t>(wave);
			image[record + 2] = static_cast<uint8_t>(0xa0u | (wave & 0x0fu));
			image[record + 3] = static_cast<uint8_t>(0x40u | (wave & 0x3fu));
			image[record + 4] = 'W';
			image[record + 5] = static_cast<uint8_t>('0' + wave / 10u);
			image[record + 6] = static_cast<uint8_t>('0' + wave % 10u);
			image[record + 7] = '!';
			for(uint32_t word = 0; word < g_canonicalPayloadWords; ++word)
			{
				const auto value = waveformWord(wave, word) & 0x00ffffffu;
				const auto offset = record + g_canonicalHeaderBytes + word * 3u;
				image[offset] = static_cast<uint8_t>(value >> 16u);
				image[offset + 1] = static_cast<uint8_t>(value >> 8u);
				image[offset + 2] = static_cast<uint8_t>(value);
			}
		}
		return image;
	}

	uint32_t imageWord(const std::vector<uint8_t>& _image,
		const uint32_t _wave, const uint32_t _word)
	{
		const auto offset = g_canonicalSourceBase + _wave * g_canonicalRecordBytes
			+ g_canonicalHeaderBytes + _word * 3u;
		return static_cast<uint32_t>(_image[offset]) << 16u
			| static_cast<uint32_t>(_image[offset + 1]) << 8u
			| static_cast<uint32_t>(_image[offset + 2]);
	}

	bool testStrictSupportedImages()
	{
		using md::MachineModel;
		if(!check(md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMd163Fnv1a, MachineModel::Machinedrum),
			"Machinedrum 1.63 fingerprint was rejected")
			|| !check(md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMm132bFnv1a, MachineModel::Monomachine),
				"Monomachine 1.32b fingerprint was rejected")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize - 1u,
				g_canonicalMm132bFnv1a, MachineModel::Monomachine),
				"incomplete Monomachine image was accepted")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize + 1u,
				g_canonicalMd163Fnv1a, MachineModel::Machinedrum),
				"oversized Machinedrum image was accepted")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMm132bFnv1a, MachineModel::Machinedrum),
				"Monomachine image was accepted for Machinedrum")
			|| !check(!md::RomLoader::isSupportedImage(g_canonicalRomSize,
				g_canonicalMd163Fnv1a, MachineModel::Monomachine),
				"Machinedrum image was accepted for Monomachine"))
			return false;

		std::vector<uint8_t> unsupported(g_canonicalRomSize, 0xff);
		return check(!md::RomLoader::isRomForModel(
				unsupported, MachineModel::Machinedrum),
			"unsupported complete Machinedrum image was accepted")
			&& check(!md::RomLoader::isRomForModel(
				unsupported, MachineModel::Monomachine),
			"unsupported complete Monomachine image was accepted");
	}

	bool testFactoryWaveforms()
	{
		using namespace md::mmwaveforms;
		if(!check(g_sourceBase == g_canonicalSourceBase
				&& g_recordBytes == g_canonicalRecordBytes
				&& g_headerBytes == g_canonicalHeaderBytes
				&& g_waveformCount == g_canonicalWaveformCount
				&& g_wordsPerWave == g_canonicalPayloadWords
				&& g_destinationBase == g_canonicalDestinationBase
				&& g_destinationStride == g_canonicalTransferWords,
			"factory waveform layout differs from the MM 1.32b firmware"))
			return false;

		auto image = makeFactoryImage();
		std::vector<std::pair<uint32_t, uint32_t>> writes;
		const auto capture = [&writes](const uint32_t _address, const uint32_t _value)
		{
			writes.emplace_back(_address, _value);
		};
		if(!check(md::mmwaveforms::loadFactoryBank(image, capture),
			"valid MKII factory waveform bank was rejected"))
			return false;

		const auto expectedWrites = static_cast<size_t>(g_canonicalWaveformCount)
			* g_canonicalTransferWords;
		if(!check(writes.size() == expectedWrites,
			"factory loader did not initialize all 64 complete waveform slots"))
			return false;

		for(uint32_t wave = 0; wave < g_canonicalWaveformCount; ++wave)
		{
			for(uint32_t word = 0; word < g_canonicalTransferWords; ++word)
			{
				const auto index = static_cast<size_t>(wave) * g_canonicalTransferWords + word;
				const auto expectedAddress = g_canonicalDestinationBase
					+ wave * g_canonicalTransferWords + word;
				const auto expectedValue = imageWord(image, wave, word);
				if(writes[index].first != expectedAddress
					|| writes[index].second != expectedValue)
					return check(false, "factory waveform address or value mismatch");
			}
		}
		if(!check(imageWord(image, 0, g_canonicalPayloadWords) == 0x1a01a1u,
				"first spill word does not come from the next record header")
			|| !check(imageWord(image, g_canonicalWaveformCount - 1u,
				g_canonicalTransferWords - 1u) == 0x00ffffffu,
				"last factory slot does not preserve erased-flash spill"))
			return false;

		writes.clear();
		auto incomplete = image;
		incomplete.resize(g_canonicalSourceBase
			+ g_canonicalWaveformCount * g_canonicalRecordBytes
			+ g_transferSpillBytes - 1u);
		if(!check(!md::mmwaveforms::loadFactoryBank(incomplete, capture),
				"incomplete factory waveform bank was accepted")
			|| !check(writes.empty(),
				"incomplete factory bank partially modified DSP memory"))
			return false;

		const auto badRecord = g_canonicalSourceBase + 37u * g_canonicalRecordBytes;
		image[badRecord + 1] ^= 1u;
		return check(!md::mmwaveforms::loadFactoryBank(image, capture),
				"malformed factory waveform bank was accepted")
			&& check(writes.empty(),
				"malformed factory bank partially modified DSP memory");
	}
}

int main()
{
	if(!testStrictSupportedImages() || !testFactoryWaveforms())
		return 1;
	std::cout << "MD/MM firmware image validation: PASS\n";
	return 0;
}
