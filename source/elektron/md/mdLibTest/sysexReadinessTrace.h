#pragma once

#include "sysexPanelDriver.h"

#include <cstdio>
#include <map>

namespace md::test
{
	// Observation only: no ROM patches, firmware RAM writes, or replaced UART
	// callbacks. These measurements are candidate signals, NOT readiness gates.
	class ReadinessTrace
	{
	public:
		explicit ReadinessTrace(std::string prefix) : m_prefix(std::move(prefix)) {}
		void observe(Hardware& hardware)
		{
			++m_pcs[hardware.getUC().getPC()];
			const auto cycles = hardware.getUC().getCycles();
			if(cycles < m_next) return;
			m_next = cycles + 10'000'000; // 250 ms, emulated MCU time
			const auto panel = hardware.getFrontPanelSnapshot();
			uint64_t hash = 14695981039346656037ull;
			for(uint32_t y = 0; y < 64; ++y)
				for(uint32_t x = 0; x < 128; ++x)
					hash = (hash ^ unsigned(panel.getLcdPixel(x, y))) * 1099511628211ull;
			std::vector<std::pair<unsigned, uint32_t>> ranked;
			for(auto [pc, count] : m_pcs) ranked.emplace_back(count, pc);
			std::sort(ranked.rbegin(), ranked.rend());
			std::printf("READINESS TRACE t=%.3f midi=%u cache=%u reboot=%u flashIdle=%.3f lcd=%016llx pcs=",
				double(cycles)/40'000'000, hardware.isFirmwareMidiReady(), hardware.isFactoryFlashCacheReady(),
				hardware.isFactoryFlashReadyForReboot(), double(hardware.getUC().flashIdleCycles())/40'000'000,
				static_cast<unsigned long long>(hash));
			for(size_t i = 0; i < std::min<size_t>(5, ranked.size()); ++i)
				std::printf("%x:%u,", ranked[i].second, ranked[i].first);
			std::puts("");
			if(hash != m_lastHash)
				panelImage(hardware, m_prefix + "-" + std::to_string(m_index) + ".pgm");
			m_lastHash = hash;
			++m_index;
			m_pcs.clear();
		}
	private:
		std::string m_prefix;
		uint64_t m_next = 0, m_lastHash = 0;
		size_t m_index = 0;
		std::map<uint32_t, unsigned> m_pcs;
	};
}
