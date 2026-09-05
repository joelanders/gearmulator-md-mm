#pragma once

#include <cstdint>

namespace md::memorymap
{
	struct Range
	{
		uint32_t begin;
		uint32_t end;

		constexpr bool contains(const uint32_t _address) const
		{
			return _address >= begin && _address < end;
		}

		constexpr uint32_t size() const { return end - begin; }
		constexpr uint32_t offset(const uint32_t _address) const { return _address - begin; }
	};

	inline constexpr Range g_flashLow       {0x00000000, 0x00100000};
	inline constexpr Range g_patchBootstrap {0x00100000, 0x00200000};
	inline constexpr Range g_mainRam         {0x00200000, 0x00300000};
	inline constexpr Range g_sim             {0x00300000, 0x00310000};
	inline constexpr Range g_loaderRam       {0x00310000, 0x00400000};
	inline constexpr Range g_dsp1Hdi08       {0x00500000, 0x00500008};
	inline constexpr Range g_dsp2Hdi08       {0x00600000, 0x00600008};
	inline constexpr Range g_patchOsAlias    {0x00700000, 0x00800000};
	inline constexpr Range g_internalSram    {0x01000000, 0x01010000};
	inline constexpr Range g_flashFull       {0x10000000, 0x10800000};
	// SFX-60 MKII user DigiPRO records occupy this 2 MiB flash window.
	inline constexpr Range g_mmUserFlash     {0x10200000, 0x10400000};
	inline constexpr Range g_mainHighAlias   {0x20000000, 0x20100000};
	inline constexpr Range g_mainExecAlias   {0x40000000, 0x40100000};

	constexpr bool isPatchRam(const uint32_t _address)
	{
		return g_patchBootstrap.contains(_address) || g_patchOsAlias.contains(_address);
	}

	constexpr uint32_t canonicalMainAddress(const uint32_t _address)
	{
		if(g_mainHighAlias.contains(_address))
			return g_mainRam.begin + g_mainHighAlias.offset(_address);
		if(g_mainExecAlias.contains(_address))
			return g_mainRam.begin + g_mainExecAlias.offset(_address);
		return _address;
	}

	constexpr uint32_t mainOffset(const uint32_t _address)
	{
		const auto canonical = canonicalMainAddress(_address);
		return g_mainRam.contains(canonical) ? g_mainRam.offset(canonical) : UINT32_MAX;
	}
}
