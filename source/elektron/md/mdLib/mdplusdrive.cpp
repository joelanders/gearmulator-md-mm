#include "mdplusdrive.h"
#include "mdsim.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace md
{
	namespace
	{
		constexpr uint8_t g_clockBit = 0x04;
		constexpr uint8_t g_dataBit = 0x08;
		// Optional read-side packet framing, kept as a force-on diagnostic
		// (GEARMULATOR_PLUSDRIVE_RFRAME). The 0x52 sector-data path is a continuous
		// nibble stream: the firmware inserts status polls at block boundaries
		// rather than reading an extra packet trailer. The 0x4c/0x63 response helpers
		// keep their established framing, a separate MD/MM boot-and-audio contract.
		constexpr uint32_t g_readPacketNibbles = 1040;
		constexpr uint32_t g_readDataNibbles = 1024;
		// OS 1.63's MD Snapshot reader runs from firmware copied into internal
		// SRAM; its 128-byte block status poll is the loop at 0x100099c. Named PC
		// window rather than raw addresses.
		constexpr uint32_t g_mdSnapshotPollBegin = 0x01000990;
		constexpr uint32_t g_mdSnapshotPollEnd = 0x010009c8;
		// MM 1.32b uses the same internal-SRAM blob at another entry point, and
		// its 0x52 receive loop takes two status reads at 0x10000c0/0x10000cc before
		// resuming the nibble stream. The trace PC is two bytes into each
		// instruction here, so the windows cover the observed 0x10000c2/0x10000ce.
		constexpr uint32_t g_mmSnapshotPollBegin = 0x010000c0;
		constexpr uint32_t g_mmSnapshotPollEnd = 0x010000d4;
		// The MM ROM-resident 0x216d... receiver has the same two-poll shape
		// at a different PC window. Its status reads are likewise transport
		// control, not bytes from the sector stream.
		constexpr uint32_t g_mmRomSnapshotPollBegin = 0x0216da0;
		constexpr uint32_t g_mmRomSnapshotPollEnd = 0x0216db8;
		constexpr uint32_t g_maxWriteNibbles = 32u * plusdrive::g_sectorSize * 2u;
		constexpr uint32_t g_formatEraseSectors = 0x400;
		constexpr uint64_t g_storeByteCount =
			static_cast<uint64_t>(plusdrive::g_sectorCount) * plusdrive::g_sectorSize;

		struct ProtocolConfig
		{
			bool readLog = false;
			bool readFrame = false;
			bool prepared = false;
			bool allZero = false;
			bool nibbleSwap = false;
			bool topZero = false;
			bool bit3One = false;
			bool fourCOnes = false;
			bool fourCOne = false;
			bool commandLog = false;
			bool phaseLog = false;
			bool writePhaseLog = false;
			bool bulkLog = false;
			bool writeLog = false;
			bool allowUnknown = false;
		};

		const ProtocolConfig g_protocolConfig = []
		{
			const auto present = [](const char* const name)
			{
				return std::getenv(name) != nullptr;
			};
			return ProtocolConfig{
				present("GEARMULATOR_PLUSDRIVE_RLOG"),
				present("GEARMULATOR_PLUSDRIVE_RFRAME"),
				present("GEARMULATOR_PLUSDRIVE_PREPARED"),
				present("GEARMULATOR_PLUSDRIVE_ALLZERO"),
				present("GEARMULATOR_PLUSDRIVE_NIBSWAP"),
				present("GEARMULATOR_PLUSDRIVE_TOPZERO"),
				present("GEARMULATOR_PLUSDRIVE_BIT3_ONE"),
				present("GEARMULATOR_PLUSDRIVE_4C_ONES"),
				present("GEARMULATOR_PLUSDRIVE_4C_ONE"),
				present("GEARMULATOR_PLUSDRIVE_CMDLOG"),
				present("GEARMULATOR_PLUSDRIVE_PHASELOG"),
				present("GEARMULATOR_PLUSDRIVE_WPHASELOG"),
				present("GEARMULATOR_PLUSDRIVE_BULKLOG"),
				present("GEARMULATOR_PLUSDRIVE_WLOG"),
				present("GEARMULATOR_PLUSDRIVE_ALLOW_UNKNOWN")};
		}();

		bool knownCommand(const uint8_t _opcode)
		{
			switch(_opcode)
			{
			case 0x40:
			case 0x41:
			case 0x42:
			case 0x43:
			case 0x46:
			case 0x47:
			case 0x49:
			case 0x4c:
			case 0x50:
			case 0x52:
			case 0x59:
			case 0x63:
			case 0x64:
			case 0x66:
				return true;
			default:
				return false;
			}
		}

		bool validByteAddress(const uint64_t _address)
		{
			return _address < g_storeByteCount;
		}

		std::string canonicalPath(const std::string& _path)
		{
			if(_path.empty())
				return {};

			std::string path = _path;
			for(char& c : path)
			{
				if(c == '\\')
					c = '/';
			}
			const bool hasDrive = path.size() > 1 && path[1] == ':';
			const bool absolute = !hasDrive && !path.empty() && path.front() == '/';
			if(!hasDrive && !absolute)
			{
				const auto current = baseLib::filesystem::getCurrentDirectory();
				path = current.empty() ? path : current + "/" + path;
			}

			std::string prefix;
			size_t start = 0;
			if(hasDrive)
			{
				prefix = path.substr(0, 2);
				start = path.size() >= 3 && path[2] == '/' ? 3 : 2;
			}
			else if(absolute)
			{
				prefix = "/";
				start = 1;
			}

			std::vector<std::string> parts;
			while(start <= path.size())
			{
				const auto end = path.find('/', start);
				const auto part = path.substr(start, end == std::string::npos
					? std::string::npos : end - start);
				if(!part.empty() && part != ".")
				{
					if(part == ".." && !parts.empty() && parts.back() != "..")
						parts.pop_back();
					else if(!absolute || part != "..")
						parts.push_back(part);
				}
				if(end == std::string::npos)
					break;
				start = end + 1;
			}

			std::string result = prefix;
			for(size_t i = 0; i < parts.size(); ++i)
			{
				if(!result.empty() && result.back() != '/')
					result += '/';
				result += parts[i];
			}
			if(result.empty())
				result = ".";
#ifdef _WIN32
			for(char& c : result)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
			return result;
		}

	}

	class DriveStore final : public std::enable_shared_from_this<DriveStore>
	{
	public:
		static std::shared_ptr<DriveStore> createMemory()
		{
			return std::make_shared<DriveStore>(std::string{}, plusdrive::Pages{},
				std::vector<uint8_t>(plusdrive::g_configSize, 0xff));
		}

		static std::shared_ptr<DriveStore> createPrimary(const std::string& _path,
			std::string* _error)
		{
			if(_path.empty())
				return createMemory();
			plusdrive::Pages pages;
			std::vector<uint8_t> config(plusdrive::g_configSize, 0xff);
			if(!plusdrive::ensureReady(pages, _path, _error, &config))
				return {};
			return std::make_shared<DriveStore>(_path, std::move(pages), std::move(config));
		}

		static std::shared_ptr<DriveStore> acquire(const std::string& _path,
			const bool _prepared, std::string* _error)
		{
			if(_path.empty())
				return createMemory();
			const auto key = canonicalPath(_path);
			std::lock_guard<std::mutex> lock(g_registryMutex);
			const auto it = g_registry.find(key);
			if(it != g_registry.end())
			{
				if(auto primary = it->second.lock())
					return _prepared ? primary->fork() : primary;
			}
			auto primary = createPrimary(key, _error);
			if(!primary)
				return {};
			g_registry[key] = primary;
			// Even when no committed owner exists yet, prepared Hardware is
			// speculative and receives an overlay; it cannot publish on its own.
			return _prepared ? primary->fork() : primary;
		}

		std::shared_ptr<DriveStore> fork()
		{
			auto overlay = std::make_shared<DriveStore>();
			std::lock_guard<std::mutex> lock(m_mutex);
			overlay->m_path = m_path;
			overlay->m_parent = shared_from_this();
			overlay->m_overlay = true;
			overlay->m_baseGeneration = m_generation;
			overlay->m_lastFlush = std::chrono::steady_clock::now();
			return overlay;
		}

		bool canCommitFrom(const std::shared_ptr<DriveStore>& _prepared,
			std::string* _error = nullptr) const
		{
			if(!_prepared)
				return false;
			bool hasChanges = false;
			uint64_t baseGeneration = 0;
			{
				std::lock_guard<std::mutex> lock(_prepared->m_mutex);
				if(!_prepared->m_overlay || _prepared->m_parent.get() != this)
				{
					if(_error)
						*_error = "prepared +Drive overlay does not belong to this committed store";
					return false;
				}
				hasChanges = !_prepared->m_pages.empty()
					|| _prepared->m_configDirty
					|| _prepared->m_eraseAll
					|| !_prepared->m_erasedSectors.empty();
				baseGeneration = _prepared->m_baseGeneration;
			}
			if(!hasChanges)
				return true;
			std::lock_guard<std::mutex> lock(m_mutex);
			if(m_generation != baseGeneration)
			{
				if(_error)
					*_error = "prepared +Drive store is stale; discard and retry it";
				return false;
			}
			return true;
		}

		bool commitFrom(const std::shared_ptr<DriveStore>& _prepared,
			std::string* _error = nullptr)
		{
			if(!_prepared || _prepared.get() == this)
				return true;
			if(m_overlay)
			{
				if(_error)
					*_error = "cannot commit a prepared overlay into another overlay";
				return false;
			}

			plusdrive::Pages pages;
			std::set<uint32_t> erasedSectors;
			bool eraseAll = false;
			std::vector<uint8_t> config;
			bool configDirty = false;
			uint64_t baseGeneration = 0;
			plusdrive::Pages previousPages;
			std::vector<uint8_t> previousConfig;
			uint64_t previousGeneration = 0;
			bool previousWriteDirty = false;
			bool previousConfigDirty = false;
			std::chrono::steady_clock::time_point previousFlush;
			{
				std::lock_guard<std::mutex> lock(_prepared->m_mutex);
				if(!_prepared->m_overlay || _prepared->m_parent.get() != this)
				{
					if(_error)
						*_error = "prepared +Drive overlay does not belong to this committed store";
					return false;
				}
				pages = _prepared->m_pages;
				erasedSectors = _prepared->m_erasedSectors;
				eraseAll = _prepared->m_eraseAll;
				config = _prepared->m_config;
				configDirty = _prepared->m_configDirty;
				baseGeneration = _prepared->m_baseGeneration;
			}
			if(pages.empty() && erasedSectors.empty() && !eraseAll && !configDirty)
				return true;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				// A prepared overlay is optimistic. Do not silently overwrite a
				// live write made after the fork; the caller must rebase/retry.
				if(m_generation != baseGeneration)
				{
					if(_error)
						*_error = "prepared +Drive store is stale; discard and retry it";
					return false;
				}
				previousPages = m_pages;
				previousConfig = m_config;
				previousGeneration = m_generation;
				previousWriteDirty = m_writeDirty;
				previousConfigDirty = m_configDirty;
				previousFlush = m_lastFlush;
				if(eraseAll)
				{
					m_pages.clear();
					m_config.assign(plusdrive::g_configSize, 0xff);
					m_configDirty = true;
				}
				else
				{
					for(const uint32_t sector : erasedSectors)
						m_pages.erase(sector);
				}
				for(const auto& entry : pages)
					m_pages[entry.first] = entry.second;
				if(configDirty)
				{
					m_config = std::move(config);
					m_configDirty = true;
				}
				if(!pages.empty() || !erasedSectors.empty() || eraseAll || configDirty)
				{
					++m_generation;
					m_publishedGeneration.store(m_generation, std::memory_order_release);
					m_writeDirty = !m_path.empty();
				}
			}
			// A prepared state is not accepted until the merged image is durable. Roll
			// back the in-memory generation on I/O failure so a failed Device commit
			// cannot look successful or leak data into the still-live machine.
			if(!flush(true))
			{
				std::string failure;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					failure = m_lastError.empty()
						? "prepared +Drive commit could not be persisted" : m_lastError;
					m_pages = std::move(previousPages);
					m_config = std::move(previousConfig);
					m_generation = previousGeneration;
					m_publishedGeneration.store(previousGeneration, std::memory_order_release);
					m_writeDirty = previousWriteDirty;
					m_configDirty = previousConfigDirty;
					m_lastError = failure;
					m_lastFlush = previousFlush;
				}
				if(_error)
					*_error = failure;
				return false;
			}
			return true;
		}

		uint8_t readByte(const uint64_t _byteAddr) const
		{
			const uint32_t sector = static_cast<uint32_t>(_byteAddr >> 9);
			const size_t offset = static_cast<size_t>(_byteAddr & (plusdrive::g_sectorSize - 1));
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto it = m_pages.find(sector);
			if(it != m_pages.end())
				return it->second[offset];
			if(m_eraseAll || m_erasedSectors.find(sector) != m_erasedSectors.end())
				return 0xff;
			if(m_parent)
				return m_parent->readByte(_byteAddr);
			return 0xff;
		}

		bool writeByte(const uint64_t _byteAddr, const uint8_t _value)
		{
			const uint32_t sector = static_cast<uint32_t>(_byteAddr >> 9);
			const size_t offset = static_cast<size_t>(_byteAddr & (plusdrive::g_sectorSize - 1));
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_pages.find(sector);
			if(it == m_pages.end())
			{
				plusdrive::Page page;
				page.fill(0xff);
				if(m_parent && !m_eraseAll
					&& m_erasedSectors.find(sector) == m_erasedSectors.end())
					page = m_parent->readPage(sector);
				it = m_pages.emplace(sector, page).first;
			}
			m_erasedSectors.erase(sector);
			it->second[offset] = _value;
			if(m_overlay)
			{
				// Overlay readers use a composite local/parent generation token;
				// advance the local half so a cached page cannot survive a
				// speculative write.
				m_publishedGeneration.fetch_add(1, std::memory_order_release);
			}
			else
			{
				++m_generation;
				m_publishedGeneration.store(m_generation, std::memory_order_release);
				m_writeDirty = !m_path.empty();
			}
			if(sector == g_configSector && offset < plusdrive::g_configSize)
			{
				if(m_config.size() != plusdrive::g_configSize)
				{
					m_config = m_parent ? m_parent->configBlock()
						: std::vector<uint8_t>(plusdrive::g_configSize, 0xff);
				}
				m_config[offset] = _value;
				m_configDirty = true;
			}
			return true;
		}

		std::vector<uint8_t> configBlock() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if(!m_config.empty())
				return m_config;
			if(m_parent)
				return m_parent->configBlock();
			return std::vector<uint8_t>(plusdrive::g_configSize, 0xff);
		}

		bool flush(const bool _force)
		{
			if(m_overlay || m_path.empty())
				return true;
			std::lock_guard<std::mutex> flushLock(m_flushMutex);
			static const uint32_t intervalMs = []
			{
				const char* const e = std::getenv("GEARMULATOR_PLUSDRIVE_FLUSHMS");
				return e != nullptr ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 500u;
			}();

			plusdrive::Pages pages;
			std::vector<uint8_t> config;
			uint64_t generation = 0;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if(!m_writeDirty && !m_configDirty)
					return m_lastError.empty();
				if(!m_lastError.empty() && !_force)
					return false;
				const auto now = std::chrono::steady_clock::now();
				if(!_force && intervalMs > 0)
				{
					const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
						now - m_lastFlush).count();
					if(elapsed >= 0 && static_cast<uint64_t>(elapsed) < intervalMs)
						return true;
				}
				// Throttle failed retries as well as successful writes. Dirty state
				// remains set, so a later event can retry after this interval.
				m_lastFlush = now;
				try
				{
					pages = m_pages;
					config = m_config;
				}
				catch(const std::exception& e)
				{
					m_lastError = std::string("cannot snapshot +Drive backing store: ") + e.what();
					return false;
				}
				catch(...)
				{
					m_lastError = "cannot snapshot +Drive backing store";
					return false;
				}
				generation = m_generation;
			}

			std::string error;
			if(!plusdrive::save(m_path, pages, config, &error))
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_lastError = error.empty() ? "cannot save +Drive backing store" : error;
				return false;
			}

			std::lock_guard<std::mutex> lock(m_mutex);
			++m_backingSaves;
			m_lastError.clear();
			// A write racing the snapshot keeps the newer generation dirty.
			if(generation == m_generation)
			{
				m_writeDirty = false;
				m_configDirty = false;
			}
			return true;
		}

		uint32_t backingSaves() const
		{
			if(m_parent)
				return m_parent->backingSaves();
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_backingSaves;
		}

		std::string persistenceError() const
		{
			if(m_parent)
				return m_parent->persistenceError();
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_lastError;
		}

		bool configDirty() const
		{
			if(m_parent)
				return m_parent->configDirty();
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_configDirty;
		}

		uint64_t generation() const
		{
			if(m_parent)
				return m_parent->generation();
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_generation;
		}

		std::string path() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_path;
		}

	public:
		uint64_t publishedGeneration() const noexcept
		{
			const uint64_t local = m_publishedGeneration.load(std::memory_order_acquire);
			if(m_parent)
			{
				// Keep the parent's generation in the token without taking its
				// map lock. The local half changes on overlay writes/erases;
				// a parent commit therefore invalidates every derived reader.
				return m_parent->publishedGeneration()
					^ (local * UINT64_C(0x9e3779b97f4a7c15));
			}
			return local;
		}

		bool eraseRange(const uint32_t _first, const uint32_t _count)
		{
			if(_count == 0)
				return true;
			const uint64_t first = static_cast<uint64_t>(_first) * plusdrive::g_sectorSize;
			const uint64_t end = std::min<uint64_t>(
				g_storeByteCount, first + static_cast<uint64_t>(_count) * plusdrive::g_sectorSize);
			if(first >= end)
				return false;
			const uint32_t firstSector = static_cast<uint32_t>(first / plusdrive::g_sectorSize);
			const uint32_t endSector = static_cast<uint32_t>(
				(end + plusdrive::g_sectorSize - 1) / plusdrive::g_sectorSize);
			std::lock_guard<std::mutex> lock(m_mutex);
			if(m_overlay)
			{
				for(uint32_t sector = firstSector; sector < endSector; ++sector)
				{
					m_erasedSectors.insert(sector);
					m_pages.erase(sector);
				}
			}
			else
			{
				m_pages.erase(m_pages.lower_bound(firstSector), m_pages.lower_bound(endSector));
			}
			const bool erasesConfig = firstSector <= g_configSector
				&& g_configSector < endSector;
			if(erasesConfig)
			{
				m_config.assign(plusdrive::g_configSize, 0xff);
				m_configDirty = true;
			}
			if(m_overlay)
			{
				m_publishedGeneration.fetch_add(1, std::memory_order_release);
				return true;
			}
			++m_generation;
			m_publishedGeneration.store(m_generation, std::memory_order_release);
			m_writeDirty = !m_path.empty();
			return true;
		}

		plusdrive::Page readPage(const uint32_t _sector) const
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				const auto it = m_pages.find(_sector);
				if(it != m_pages.end())
					return it->second;
				if(m_eraseAll || m_erasedSectors.find(_sector) != m_erasedSectors.end())
				{
					plusdrive::Page page;
					page.fill(0xff);
					return page;
				}
			}
			if(m_parent)
				return m_parent->readPage(_sector);
			plusdrive::Page page;
			page.fill(0xff);
			return page;
		}

		DriveStore(std::string _path, plusdrive::Pages _pages,
			std::vector<uint8_t> _config)
			: m_path(std::move(_path))
			, m_pages(std::move(_pages))
			, m_config(std::move(_config))
		{
		}

		DriveStore() = default;
		~DriveStore()
		{
			try
			{
				if(!flush(true) && !m_path.empty())
					std::fprintf(stderr, "[PLUSDRIVE] final save failed for %s: %s\n",
						m_path.c_str(), m_lastError.c_str());
			}
			catch(...)
			{
			}
		}

	private:
		static constexpr uint32_t g_configSector = 0x1fffff;
		std::string m_path;
		plusdrive::Pages m_pages;
		std::vector<uint8_t> m_config;
		std::shared_ptr<DriveStore> m_parent;
		uint64_t m_generation = 1;
		std::atomic<uint64_t> m_publishedGeneration{1};
		uint64_t m_baseGeneration = 0;
		bool m_overlay = false;
		bool m_eraseAll = false;
		std::set<uint32_t> m_erasedSectors;
		bool m_writeDirty = false;
		bool m_configDirty = false;
		mutable std::mutex m_mutex;
		std::mutex m_flushMutex;
		std::chrono::steady_clock::time_point m_lastFlush = std::chrono::steady_clock::now();
		uint32_t m_backingSaves = 0;
		std::string m_lastError;

		static inline std::mutex g_registryMutex;
		static inline std::map<std::string, std::weak_ptr<DriveStore>> g_registry;
	};

	PlusDrive::PlusDrive()
		: m_store(DriveStore::createMemory())
	{
	}

	PlusDrive::PlusDrive(PlusDrive&&) noexcept = default;
	PlusDrive& PlusDrive::operator=(PlusDrive&&) noexcept = default;

	uint8_t PlusDrive::dataBitForRead(const uint8_t _ppdatLatch, const uint32_t _pc)
	{
		// Read-side boundary trace (GEARMULATOR_PLUSDRIVE_RLOG=1): logs the
		// engine PCs and served levels at packet boundaries, where the trailer
		// has to line up with the firmware handshake/status sampling.
		const uint8_t value = dataBitForReadImpl(_ppdatLatch, _pc);
		if(g_protocolConfig.readLog
			&& m_lastCmdValid && _pc >= 0x2401f0 && _pc <= 0x2402e0)
		{
			const uint32_t inPacket = m_bulkPos % g_readPacketNibbles;
			if(inPacket < 24 || inPacket > g_readDataNibbles - 24)
				std::fprintf(stderr, "[RLOG] pc=%06x cmd=%02x pos=%u inPacket=%u ret=%02x\n",
					_pc, m_lastCmd[0], m_bulkPos, inPacket, value);
		}
		return value;
	}

	uint8_t PlusDrive::dataBitForReadImpl(const uint8_t _ppdatLatch, const uint32_t _pc)
	{
		(void)_ppdatLatch;
		(void)_pc;

		// Drive levels cover bits 3-7 (see Sim::computeParallelData):
		// bit3 = presence/data bit, bits 4-7 = data bus. Bits 0-2 are never
		// driven (mask 0xF8).
		if(m_mode == Mode::AlwaysHigh)
			return 0xF8;

		if(m_mode == Mode::AlwaysLow)
		{
			// Drive low constantly (experiment baseline; wedges bge-high
			// waits — use Canned for full boot).
			return 0x00;
		}

		// Canned mode: a command-state machine replaces PC-gating.
		// the last decoded 6-byte transmit selects the receive levels.
		// No command yet (early boot MKII/battery checks) -> idle-high,
		// i.e. absent behavior, so early boot is untouched.
		if(!m_lastCmdValid)
			return 0xF8;
		++m_cmdReadPos;
		++m_cmdStats[m_lastCmd[0]];
		switch(m_lastCmd[0])
		{
		case 0x41:
			// Presence probe: upper high, bit3 = 8-zero preamble + magic.
			{
				const bool bit = cannedBitAt(m_cannedBitPos++, 0xc0ff8080);
				return static_cast<uint8_t>(0xF0 | (bit ? 0x08 : 0x00));
			}
		case 0x40:
		case 0x52:
		{
			// Init data phases: first reads are the upper-nibble poll
			// (drive low for immediate success), then bulk data.
			// Top probe ($2405be reads 0x52 @ 0x001FFFFF for $262404):
			// serve the GF(2)-solved first-word sequence for 0x4E450000
			// (prepared-state word) on reads 2-9, then flash.
			if(m_lastCmd[0] == 0x52 && m_lastCmd[1] == 0x00
				&& m_lastCmd[2] == 0x1f && m_lastCmd[3] == 0xff
				&& m_lastCmd[4] == 0xff)
			{
				// Top probe / boot state word: `$200f30` stores the returned longword into
				// `$262404`, so this read decides the reboot gate. Genuine read-after-write:
				// the firmware streams the config block to 0x1FFFFF via a `0x59` command plus
				// an `0xf4` bulk phase, and the first 16 bytes persist in the config
				// trailer. Serving the same offset back round-trips 0x4E450007 on a prepared
				// image and 0xFFFFFFFF (erased) on a fresh one, so the first boot prepares
				// and reboots and the second reaches the Kit UI. First read is the
				// upper-nibble poll (drive low).
				if(m_cmdReadPos <= 1)
					return 0x00;
				// Test shortcut: force the prepared word 0x4E450007 without
				// seeding the backing file (reads 2-9 = high nibble per read byte).
				if(g_protocolConfig.prepared
					&& m_cmdReadPos <= 9)
				{
					const uint8_t kTop[8] = {
						0x40, 0xe0, 0x40, 0x50, 0x00, 0x00, 0x00, 0x70 };
					return kTop[m_cmdReadPos - 2];
				}
				// Serve the persisted config block, nibble-framed high-first:
				// read 2 -> high nibble of byte 0, read 3 -> low nibble, ...
				// (matches the experimentally-established kTop framing).
				{
					const auto config = configBlock();
					const uint32_t nibIdx = (m_cmdReadPos - 2) % (plusdrive::g_configSize * 2);
					const uint8_t byte = config[nibIdx / 2];
					const uint8_t nib = (nibIdx % 2) == 0
						? static_cast<uint8_t>((byte >> 4) & 0x0f)
						: static_cast<uint8_t>(byte & 0x0f);
					// Nibble sits in bits 4-7 with bit3 (data) low, exactly like
					// the established kTop framing (0x40/0xe0/... for 0x4E450007).
					return static_cast<uint8_t>(nib << 4);
				}
			}
			// Pre-data reads: the firmware consumes exactly ONE read before it packs
			// nibble pairs, which the top probe above makes concrete (its 8 reads, 2..9,
			// are the 8 nibbles of 4 bytes MSB-first). Burning two slipped every
			// transfer by one nibble, so after a boot byte j at `$2b2736` carried the
			// sector's nibbles (2j-1, 2j) and no directory entry ever held the 0xAB
			// validity byte (`SNAPSHOTS: 000`). Default 1;
			// GEARMULATOR_PLUSDRIVE_RPOLL=N restores the old count.
			static const uint32_t rpoll = []{
				const char* e = std::getenv("GEARMULATOR_PLUSDRIVE_RPOLL");
				return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 1u;
			}();
			if(m_cmdReadPos <= rpoll)
				return 0x00;
			// The MD 16-KiB internal-SRAM reader polls the upper bus before each
			// 128-byte block (0x100099c). That is a status read, not a flash nibble.
			// Serving it from the storage stream inserts an extra nibble at every block
			// boundary, and the first is enough to shift the Snapshot marker.
			const bool snapshotPoll = m_lastCmd[0] == 0x52
				&& ((m_model == MachineModel::Machinedrum
						&& _pc >= g_mdSnapshotPollBegin && _pc < g_mdSnapshotPollEnd)
					|| (m_model == MachineModel::Monomachine
						&& ((_pc >= g_mmSnapshotPollBegin && _pc < g_mmSnapshotPollEnd)
							|| (_pc >= g_mmRomSnapshotPollBegin && _pc < g_mmRomSnapshotPollEnd))));
			if(snapshotPoll)
				return 0x00;
			if(m_serveFlash)
				return serveFlashNibble(g_protocolConfig.readFrame);
			{
				const bool bit = cannedBitAt(m_cannedBitPos++, 0xc0ff8080);
				return static_cast<uint8_t>(0xF0 | (bit ? 0x08 : 0x00));
			}
		}
		case 0x4c:
			// Experiment (GEARMULATOR_PLUSDRIVE_4C_ONES=1): status reads high
			// (erased) to probe the blank/format branch.
			if(g_protocolConfig.fourCOnes)
				return 0xF8;
			// Experiment (GEARMULATOR_PLUSDRIVE_4C_ONE=1): $23ea86 has no
			// wait loop (7 D6 reads + 32 D5 reads), so serve 7-zero preamble
			// + D5==1 to probe the status==1 branch.
			if(g_protocolConfig.fourCOne)
			{
				const bool bit = cannedBitAt(m_cannedBitPos++, 0x00000001, 7);
				return static_cast<uint8_t>(0xF0 | (bit ? 0x08 : 0x00));
			}
			if(m_serveFlash)
				return serveFlashNibble(true);
			{
				const bool bit = cannedBitAt(m_cannedBitPos++, 0xc0ff8080);
				return static_cast<uint8_t>(0xF0 | (bit ? 0x08 : 0x00));
			}
		case 0x63:
		case 0x64:
		case 0x66:
			// Snapshot-bank flow ($23f2aa via $23ea86): D5 reads start one later than
			// the D6 count suggests (D0PC measured D5=0x1200 = payload<<1 with preamble
			// 7), so use preamble 8: eight zeros then D5==0x900 MSB-first. Bulk after the
			// 40 transfer reads serves flash (H0). pos advances twice per read: transfer
			// spans 1..79, bulk starts at 81.
			{
				// m_cmdReadPos pre-incremented on entry, once more here.
				const uint32_t pos = m_cmdReadPos++;
				if(pos < 81)
				{
					const bool bit = cannedBitAt(m_cannedBitPos++, 0x00000900, 8);
					return static_cast<uint8_t>(0xF0 | (bit ? 0x08 : 0x00));
				}
				if(m_serveFlash)
					return serveFlashNibble(true);
				const bool bit = cannedBitAt(m_cannedBitPos++, 0x00000900, 8);
				return static_cast<uint8_t>(0xF0 | (bit ? 0x08 : 0x00));
			}
		case 0x59:
			// Status phase. The receive routine is firmware-specific: MD 1.63 samples
			// three status bits from bit 7, MM 1.32b adds a second handshake with its own
			// pre-wait and sample PCs. Keep the sequenced status payload separate from
			// the read-side packet trailer: diverting 0x59 through serveFlashNibble
			// breaks SAVE, which needs the accumulated status value 2.
			{
				const bool bit3 = cannedBitAt(m_cannedBitPos++, 0x00000900);
				// m_cmdReadPos pre-incremented on entry, once more here.
				(void)m_cmdReadPos++;
				bool bit7 = true;
				// The status loop is firmware-specific. MD 1.63 samples
				// 0x240216/0x240244/0x240264; MM 1.32b has the same shape at
				// 0x217ed2/0x217f00/0x217f18, and its second handshake at 0x2180be polls
				// 0x2180da before sampling 0x218108/0x218118/0x218128. Return low on the
				// pre-wait and the first and third samples so the accumulated value is
				// 101b (5), leaving the middle sample high.
				if(_pc == 0x240216 || (m_model == MachineModel::Monomachine
					&& (_pc == 0x217ed2 || _pc == 0x2180da
						|| _pc == 0x218108 || _pc == 0x218128))
					|| _pc == 0x240244 || (m_model == MachineModel::Monomachine
					&& _pc == 0x217f00)
					|| _pc == 0x240264 || (m_model == MachineModel::Monomachine
					&& _pc == 0x217f18))
					bit7 = false;
				return static_cast<uint8_t>(0x70 | (bit7 ? 0x80 : 0x00)
					| (bit3 ? 0x08 : 0x00));
			}
		default:
			// Known read/data opcodes are the only commands allowed to use the
			// raw flash stream. Unknown/partial commands fail closed instead of
			// accidentally making a firmware wait look like a successful read.
			if((g_protocolConfig.allowUnknown || knownCommand(m_lastCmd[0]))
				&& m_serveFlash)
				return serveFlashNibble(true);
			return 0xf8;
		}
	}

	uint8_t PlusDrive::readByte(const uint64_t _byteAddr) const
	{
		if(!validByteAddress(_byteAddr) || !m_store)
			return 0xff;
		// The generation token includes both the committed parent generation
		// and the overlay's local revision, so prepared machines get the same
		// page-cache fast path without hiding a speculative or newly committed
		// write.
		const auto sector = static_cast<uint32_t>(_byteAddr >> 9);
		if(m_cachedSector != sector)
		{
			const auto generation = m_store->publishedGeneration();
			m_cachedPage = m_store->readPage(sector);
			m_cachedSector = sector;
			m_cachedGeneration = generation;
		}
		else
		{
			const auto generation = m_store->publishedGeneration();
			if(m_cachedGeneration != generation)
			{
				m_cachedPage = m_store->readPage(sector);
				m_cachedGeneration = generation;
			}
		}
		return m_cachedPage[_byteAddr & (plusdrive::g_sectorSize - 1)];
	}

	void PlusDrive::writeByte(const uint64_t _byteAddr, const uint8_t _value)
	{
		if(validByteAddress(_byteAddr) && m_store)
		{
			invalidateReadCache();
			(void)m_store->writeByte(_byteAddr, _value);
		}
	}

	uint8_t PlusDrive::serveFlashNibble(const bool _packetFraming)
	{
		// Experiment (ALLZERO=1): serve every nibble as 0 (probe whether
		// verification wants zero-filled "empty" rather than erased).
		if(g_protocolConfig.allZero)
		{
			++m_bulkPos;
			return 0x00;
		}
		// Experiment (NIBSWAP=1): low/high nibble order swapped (probe
		// silicon nibble order for pointer/magic validation).
		const bool swap = g_protocolConfig.nibbleSwap;
		// Experiment (TOPZERO=1): top flash word reads all-zero (probe the
		// 0x59 @ 0x1FFFFF ready-poll exit value).
		if(g_protocolConfig.topZero
			&& (m_bulkBase & 0x1fffff) == 0x1fffff)
		{
			++m_bulkPos;
			return 0x00;
		}
		// Wire addresses are sector numbers; see the geometry note in
		// mdplusdrive.h. RFRAME is a bisect knob, not a per-model option: both
		// measured MM 1.32b 0x52 receivers consume a continuous nibble stream
		// (status poll per 128-byte block, then 8 nibbles per longword, no
		// 8-byte trailer), so enabling it per model inserts 16 unconsumed
		// nibbles per 512-byte window and shifts the restored Snapshot payload.
		const bool rframe = _packetFraming;
		uint32_t dataNibble = m_bulkPos;
		if(rframe)
		{
			const uint32_t inPacket = m_bulkPos % g_readPacketNibbles;
			if(inPacket >= g_readDataNibbles)
			{
				++m_bulkPos;
				// Trailer nibble f; the status samples are the 1st, 3rd and 5th
				// trailer reads, so bit7 must be 1, 0, 1.
				const uint32_t f = inPacket - g_readDataNibbles;
				const bool bit7 = (f == 0) || (f == 4);
				return static_cast<uint8_t>(bit7 ? 0x80 : 0x00);
			}
			dataNibble = (m_bulkPos / g_readPacketNibbles) * g_readDataNibbles + inPacket;
		}
		const uint64_t addr = (static_cast<uint64_t>(m_bulkBase) << 9)
			+ dataNibble / 2;
		// A bulk read walks sectors sequentially. Check the shared generation
		// once at each sector/command boundary instead of once per nibble; the
		// normal emulator path is single-threaded, while the public readByte
		// helper below retains a strict per-call check for diagnostics/tests.
		uint8_t byte = 0xff;
		if(m_store && validByteAddress(addr))
		{
			const uint32_t sector = static_cast<uint32_t>(addr >> 9);
			if(m_cachedSector != sector || m_bulkPos == 0)
			{
				const uint64_t generation = m_store->publishedGeneration();
				if(m_cachedSector != sector || m_cachedGeneration != generation)
				{
					m_cachedPage = m_store->readPage(sector);
					m_cachedSector = sector;
					m_cachedGeneration = generation;
				}
			}
			if(m_cachedSector == sector)
				byte = m_cachedPage[addr & (plusdrive::g_sectorSize - 1)];
		}
		const bool highFirst = ((dataNibble % 2) == 0) != swap;
		const uint8_t nibble = highFirst
			? static_cast<uint8_t>((byte >> 4) & 0x0f)
			: static_cast<uint8_t>(byte & 0x0f);
		++m_bulkPos;
		// Experiment (BIT3_ONE=1): erased flash drives bit3 high too.
		const uint8_t bit3 = (g_protocolConfig.bit3One) ? 0x08 : 0x00;
		return static_cast<uint8_t>(bit3 | (nibble << 4));
	}

	PlusDrive::~PlusDrive()
	{
		try
		{
			(void)flushBacking(true);
		}
		catch(...)
		{
		}
	}

	bool PlusDrive::flushBacking(const bool _force)
	{
		return m_store ? m_store->flush(_force) : true;
	}

	uint32_t PlusDrive::backingSaves() const
	{
		return m_store ? m_store->backingSaves() : 0;
	}

	std::string PlusDrive::persistenceError() const
	{
		return m_store ? m_store->persistenceError() : std::string{};
	}

	uint64_t PlusDrive::storageGeneration() const
	{
		return m_store ? m_store->generation() : 0;
	}

	std::vector<uint8_t> PlusDrive::configBlock() const
	{
		return m_store ? m_store->configBlock()
			: std::vector<uint8_t>(plusdrive::g_configSize, 0xff);
	}

	bool PlusDrive::commitPrepared(PlusDrive& _prepared, std::string* _error)
	{
		if(_error)
			_error->clear();
		const bool committed = m_store && _prepared.m_store
			&& m_store->commitFrom(_prepared.m_store, _error);
		if(committed)
		{
			invalidateReadCache();
			_prepared.invalidateReadCache();
		}
		return committed;
	}

	bool PlusDrive::canCommitPrepared(const PlusDrive& _prepared,
		std::string* _error) const
	{
		if(_error)
			_error->clear();
		return m_store && _prepared.m_store
			&& m_store->canCommitFrom(_prepared.m_store, _error);
	}

	void PlusDrive::persistWritePage()
	{
		// m_txBytes = 6-byte command + one 512-byte page carried in the same
		// direction phase. Validate the complete operation before mutating the
		// store so an invalid address cannot partially alias another sector.
		if(m_txBytes.size() <= 6 || m_txBytes.size() - 6 > plusdrive::g_sectorSize)
			return;
		const uint32_t base = (static_cast<uint32_t>(m_txBytes[1]) << 24)
			| (static_cast<uint32_t>(m_txBytes[2]) << 16)
			| (static_cast<uint32_t>(m_txBytes[3]) << 8)
			| static_cast<uint32_t>(m_txBytes[4]);
		if(base >= plusdrive::g_sectorCount)
			return;
		const uint64_t byteBase = static_cast<uint64_t>(base) << 9;
		if(!validByteAddress(byteBase + m_txBytes.size() - 7))
			return;
		for(size_t i = 6; i < m_txBytes.size(); ++i)
			writeByte(byteBase + i - 6, m_txBytes[i]);
		++m_writePages;
		(void)flushBacking(false);
	}

	bool PlusDrive::setBackingPath(const std::string& _path, const bool _prepared,
		std::string* _error)
	{
		if(_error)
			_error->clear();
		if(m_store && canonicalPath(m_store->path()) == canonicalPath(_path) && !_prepared)
			return true;
		if(m_store && !m_store->path().empty() && !flushBacking(true))
		{
			if(_error)
				*_error = m_store->persistenceError();
			return false;
		}
		auto store = DriveStore::acquire(_path, _prepared, _error);
		if(!store)
			return false;
		m_store = std::move(store);
		invalidateReadCache();
		resetTransaction();
		return true;
	}

	bool PlusDrive::cannedBitAt(const uint32_t _pos, const uint32_t _payload,
		const uint32_t _preamble) const
	{
		// _preamble zero reads (send_raw tail: 1 wait + 7 D6 = 8; $23ea86:
		// 7 D6 with no wait loop), then 32 payload bits MSB-first. Repeats
		// per transaction.
		const uint32_t total = _preamble + 32;
		const uint32_t pos = _pos % total;
		if(pos < _preamble)
			return false;
		const uint32_t payloadPos = pos - _preamble;
		if(m_cannedOrder != CannedOrder::MsbNormal)
		{
			// Legacy brute-force orders: only defined for the presence
			// magic; other payloads use MSB-normal byte order.
			static constexpr uint8_t kMagicNormal[4] = { 0xC0, 0xFF, 0x80, 0x80 };
			static constexpr uint8_t kMagicReversed[4] = { 0x80, 0x80, 0xFF, 0xC0 };
			const bool reversed =
				m_cannedOrder == CannedOrder::MsbReversed ||
				m_cannedOrder == CannedOrder::LsbReversed;
			const bool lsb =
				m_cannedOrder == CannedOrder::LsbNormal ||
				m_cannedOrder == CannedOrder::LsbReversed;
			const uint8_t byte = (reversed ? kMagicReversed : kMagicNormal)[payloadPos / 8];
			if(lsb)
				return ((byte >> (payloadPos % 8)) & 1) != 0;
			const uint32_t bitInByte = 7 - (payloadPos % 8);
			return ((byte >> bitInByte) & 1) != 0;
		}
		return ((_payload >> (31 - payloadPos)) & 1) != 0;
	}

	void PlusDrive::onPPDDRWrite(const uint8_t _newDdr)
	{
		invalidateReadCache();
		const bool enteringBulk = m_lastDdr != 0xf4 && _newDdr == 0xf4;
		if(enteringBulk)
			m_wPhaseWrites = 0;
		// Leaving transmit: latch only a complete command (and any write-data
		// page in the same phase), then prep the receive response. An aborted
		// phase invalidates the old response instead of rewinding it.
		if(m_lastDdr == 0x0c && _newDdr != 0x0c)
		{
			m_wordsSeen += static_cast<uint32_t>(m_txBytes.size());
			const bool complete = m_txBytes.size() >= 6
				&& m_txCurBits == 0 && !m_txOverflow;
			uint32_t decodedBase = 0;
			if(complete)
			{
				decodedBase = (static_cast<uint32_t>(m_txBytes[1]) << 24)
					| (static_cast<uint32_t>(m_txBytes[2]) << 16)
					| (static_cast<uint32_t>(m_txBytes[3]) << 8)
					| static_cast<uint32_t>(m_txBytes[4]);
				for(size_t i = 0; i < 6; ++i)
					m_lastCmd[i] = m_txBytes[i];
				m_lastCmdValid = true;
				if(g_protocolConfig.commandLog)
				{
					std::fprintf(stderr,
						"[PLUSDRIVE-CMD #%u] %02x %02x %02x %02x %02x %02x"
						" (%zu bytes) prevBase=%06x prevWr=%u prevRd=%u\n",
						m_cmdSeq++,
						m_txBytes[0], m_txBytes[1], m_txBytes[2],
						m_txBytes[3], m_txBytes[4], m_txBytes[5],
						m_txBytes.size(), m_bulkBase, m_bulkWPos / 2, m_bulkPos / 2);
				}
				// 0x63/0x64 are the firmware's physical erase transactions, used by FORMAT
				// (0x63 erases a 0x400-sector block, 0x64 clears its directory sector).
				// Model/drive command decoding supplies the address, so erased sectors can
				// become truly unreadable instead of returning 0xff over old pages.
				if(m_serveFlash && m_store)
				{
					if(m_txBytes[0] == 0x63)
						(void)m_store->eraseRange(decodedBase, g_formatEraseSectors);
					else if(m_txBytes[0] == 0x64)
						(void)m_store->eraseRange(decodedBase, 1);
				}
				// Only known write commands may carry an inline page. Reads and
				// unimplemented opcodes never mutate durable storage.
				if(m_txBytes.size() > 6 && m_serveFlash
					&& (m_txBytes[0] == 0x42 || m_txBytes[0] == 0x59))
					persistWritePage();
				// Ordinary command boundaries use the coalescing interval. A
				// reset, replacement commit, or teardown forces the final state
				// word; this avoids rewriting a full image for every top-sector
				// maintenance write during one prep pass.
				(void)flushBacking(false);
			}
			else
			{
				m_lastCmdValid = false;
			}
			m_txBytes.clear();
			m_txOverflow = false;
			m_txCurByte = 0;
			m_txCurBits = 0;
			m_cannedBitPos = 0;
			m_txBitsLeft = 0;
			m_cmdReadPos = 0;
			m_bulkPos = 0;
			m_bulkWPos = 0;
			m_bulkWPhase = 0;
			m_bulkBase = decodedBase;
		}
		else if(m_lastDdr == 0xf4 && _newDdr != 0xf4)
		{
			// Do not let an odd/truncated phase pair its high nibble with the
			// first nibble of the next phase. Treat the incomplete byte as
			// discarded and keep the next phase byte-aligned.
			if(m_bulkWPos & 1)
			{
				++m_bulkWPos;
				m_txWByte = 0;
			}
			// Leaving a bulk payload phase: flush buffered writes.
			// PHASELOG prints one line per phase with BOTH directions' address
			// maps, so a failing flow (e.g. the snapshot save) can be read as
			// "wrote X..X+n, then read back Y..Y+m" without per-byte dumps.
			if(g_protocolConfig.phaseLog)
				std::fprintf(stderr,
					"[PHASE] cmd=%02x rd_base=%06x rd_bytes=%u wr_base=%06x wr_bytes=%u\n",
					m_lastCmd[0], m_bulkBase, m_bulkPos / 2, m_bulkBase, m_bulkWPos / 2);
			if(g_protocolConfig.writePhaseLog)
			{
				std::fprintf(stderr, "[WPHASE] cmd=%02x base=%06x writes=%u ops=",
					m_lastCmd[0], m_bulkBase, m_phaseWrites);
				for(uint32_t i = 0; i < m_phaseValsN; ++i)
					std::fprintf(stderr, "%02x ", m_phaseVals[i]);
				std::fprintf(stderr, " tail=");
				for(uint32_t i = 0; i < 8; ++i)
					std::fprintf(stderr, "%02x ", m_phaseTail[(m_phaseTailPos + i) % 8]);
				std::fprintf(stderr, "\n");
				m_phaseValsN = 0;
				m_phaseTailPos = 0;
				m_phaseWrites = 0;
			}
			if(g_protocolConfig.bulkLog && m_bulkWPos > 0)
			{
				std::fprintf(stderr,
					"[BULK] base=%06x bytes=%u lastCmd=%02x %02x %02x %02x %02x %02x first=",
					m_bulkBase, m_bulkWPos / 2,
					m_lastCmd[0], m_lastCmd[1], m_lastCmd[2],
					m_lastCmd[3], m_lastCmd[4], m_lastCmd[5]);
				for(int i = 0; i < 8; ++i)
					std::fprintf(stderr, "%02x ", readByte(
						(static_cast<uint64_t>(m_bulkBase) << 9) + i));
				std::fprintf(stderr, "\n");
			}
			flushBacking(false);
		}
		else if(m_lastDdr != 0xf4 && _newDdr == 0xf4)
		{
			// Entering a bulk payload phase. Every phase of a command, not just the
			// first, opens with the `f0 f4 00` prologue and ends with the `f4` dismiss
			// marker; all 4353 phases of a fresh prep run carry it (WPHASELOG `ops=`)
			// and each is 1044 writes. The phase-local counter restarts here, while
			// m_bulkWPos keeps accumulating so 32 x 512 bytes tile the command's 16 KiB
			// window (bases step 0x20 sectors). Counting per command instead pushed
			// phases 2..32 outside the budget, storing only the first sector of each.
			m_wPhaseWrites = 0;
		}
		else if(m_lastDdr != 0x0c && _newDdr == 0x0c)
		{
			m_txBytes.clear();
			m_txOverflow = false;
			m_txCurByte = 0;
			m_txCurBits = 0;
			m_lastCmdValid = false;
		}
		m_lastDdr = _newDdr;
		m_lastClock = false;
		m_txLastClock = false;
	}

	void PlusDrive::onPPDATWrite(const uint8_t _value, const uint8_t _ddr)
	{
		if(_ddr == 0x0c)
		{
			// Command phase: every PPDAT write carries one data bit MSB-first
			// (send_raw writes 8 writes per byte, one bit per write, clock in
			// bit2 + data in bit3). Sample every write.
			const bool data = (_value & g_dataBit) != 0;
			m_txCurByte = static_cast<uint8_t>((m_txCurByte << 1) | (data ? 1 : 0));
			if(++m_txCurBits >= 8)
			{
				if(m_txBytes.size() < 6 + plusdrive::g_sectorSize)
					m_txBytes.push_back(m_txCurByte);
				else
					m_txOverflow = true;
				m_txCurByte = 0;
				m_txCurBits = 0;
			}
			m_txLastClock = (_value & g_clockBit) != 0;
			return;
		}
		// A bulk phase is writable only after a complete 0x59 command. Do
		// not let a stale m_bulkBase or a foreign/truncated PPDDR=f4 phase
		// mutate the last address selected by another transaction.
		if(_ddr == 0xf4 && m_serveFlash
			&& m_lastCmdValid && m_lastCmd[0] == 0x59)
		{
			// Bulk payload phase: the firmware streams RAM to the bus with data in the
			// high nibble, mirroring the H0 read mapping so the drive is
			// self-consistent. Each phase opens with 3 setup writes (F4/F0/04 clocks, no
			// data) that must be skipped or every byte lands shifted. NIBSWAP=1 probes
			// lo-first order; reads mirror it.
			//
			// Setup-write skip is 3 at boot, but the directory read-modify-write loop
			// writes come out shifted by one byte with 3, the signature of skipping one
			// nibble pair too many, so it stays tunable
			// (GEARMULATOR_PLUSDRIVE_WSKIP=N).
			static const uint32_t wskip = []{
				const char* e = std::getenv("GEARMULATOR_PLUSDRIVE_WSKIP");
				return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 3u;
			}();
			// Payload budget per phase (GEARMULATOR_PLUSDRIVE_WPHASELEN=N, 0 =
			// unbounded). Every 0xf4 save phase is exactly 1044 writes: `F0 F4 00`
			// prologue, payload, one-write dismiss marker. One nibble per write, high
			// nibble = data, confirmed in the sender at 0x240130, so bounding the phase
			// gives a byte-exact payload instead of trailing into the marker. Default
			// 1040 = the measured frame (3 + 1040 + 1).
			static const uint32_t wpayload = []{
				const char* e = std::getenv("GEARMULATOR_PLUSDRIVE_WPHASELEN");
				return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 1040u;
			}();
			// Stored-prefix budget per phase (GEARMULATOR_PLUSDRIVE_WPBUDGET=N,
			// 0 = store the whole payload). The packet is 520 bytes (130 longs) against
			// a 512-byte sector, and the command ladder steps 0x20 = 32 sectors, so the
			// packet only tiles the space if 8 of its bytes are framing. N=1024 stores
			// 512 bytes and drops the last 8: a postamble. The two-boot contract decides
			// it, since a preamble would put the config word 8 bytes into the sector
			// while reads serve from byte 0. With 1024 the save's entry lands at sector
			// offset 480, exactly where the loader reads it (`$2b2736 + 0x1E0`), so
			// 1024 is the default.
			static const uint32_t wbudget = []{
				const char* e = std::getenv("GEARMULATOR_PLUSDRIVE_WPBUDGET");
				return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 10)) : 1024u;
			}();
			// WPHASELOG diagnostics: record the raw opening writes (including
			// the skipped ones) and the phase total, so the prologue of a phase
			// that needs skip 3 can be told apart from one that needs skip 1.
			if(m_phaseValsN < 8)
				m_phaseVals[m_phaseValsN++] = _value;
			m_phaseTail[m_phaseTailPos] = _value;
			m_phaseTailPos = (m_phaseTailPos + 1) % 8;
			++m_phaseWrites;
			++m_bulkWPhase;
			++m_wPhaseWrites;
			if(m_wPhaseWrites <= wskip)
				return;
			if(wpayload != 0 && m_wPhaseWrites > wskip + wpayload)
				return;
			if(wbudget != 0 && m_wPhaseWrites > wskip + wbudget)
				return;
			// A command's write window is 16 KiB (32 sectors). Do not let a
			// malformed/long phase wrap into a later address window.
			if(m_bulkWPos >= g_maxWriteNibbles)
				return;
			const bool swap = g_protocolConfig.nibbleSwap;
			const uint8_t nibble = static_cast<uint8_t>((_value >> 4) & 0x0f);
			if((m_bulkWPos % 2) == 0)
				m_txWByte = swap ? nibble : static_cast<uint8_t>(nibble << 4);
			else
			{
				// Same sector -> byte mapping as the read path (see serveFlashNibble).
				const uint8_t byte = swap ? static_cast<uint8_t>(m_txWByte | (nibble << 4))
					: static_cast<uint8_t>(m_txWByte | nibble);
				const uint64_t address = (static_cast<uint64_t>(m_bulkBase) << 9)
					+ m_bulkWPos / 2;
				// DriveStore updates the dedicated config trailer for the top
				// sector on every write transport, including this 0xf4 path and
				// the legacy inline command-page path.
				writeByte(address, byte);
			}
			++m_bulkWPos;
		}
	}

	void PlusDrive::resetTransaction()
	{
		invalidateReadCache();
		// A reset/replacement is a persistence barrier. In particular, do not
		// clear the state-word dirty bit merely because the firmware changed
		// direction before the normal flush interval elapsed.
		if(m_store)
			(void)m_store->flush(true);

		m_lastClock = false;
		m_shiftIn = 0;
		m_bitCount = 0;
		m_lastWord = 0;
		m_wordsSeen = 0;
		m_txBitsLeft = 0;
		m_txLastClock = false;
		m_txCurByte = 0;
		m_txCurBits = 0;
		m_txBytes.clear();
		m_txOverflow = false;
		m_lastDdr = 0;
		m_cannedBitPos = 0;
		m_lastCmdValid = false;
		m_lastCmd = {};
		m_bulkBase = 0;
		m_bulkPos = 0;
		m_cmdReadPos = 0;
		m_writePages = 0;
		m_bulkWPos = 0;
		m_txWByte = 0;
		m_bulkWPhase = 0;
		m_wPhaseWrites = 0;
		m_phaseValsN = 0;
		m_phaseTailPos = 0;
		m_phaseWrites = 0;
		m_cmdSeq = 0;
		m_cmdStats = {};
	}

	namespace plusdrive
	{
		namespace
		{
			constexpr char g_magic[10] = { 'M','D','P','L','U','S','D','R','V','1' };
			constexpr size_t g_headerSize = 16;

			void appendU16(std::vector<uint8_t>& _out, const uint16_t _v)
			{
				_out.push_back(static_cast<uint8_t>(_v >> 8));
				_out.push_back(static_cast<uint8_t>(_v & 0xff));
			}

			void appendU32(std::vector<uint8_t>& _out, const uint32_t _v)
			{
				for(int i = 3; i >= 0; --i)
					_out.push_back(static_cast<uint8_t>((_v >> (8 * i)) & 0xff));
			}

			uint16_t readU16(const std::vector<uint8_t>& _in, const size_t _off)
			{
				return static_cast<uint16_t>(
					(static_cast<uint16_t>(_in[_off]) << 8) | _in[_off + 1]);
			}

			uint32_t readU32(const std::vector<uint8_t>& _in, const size_t _off)
			{
				uint32_t v = 0;
				for(size_t i = 0; i < 4; ++i)
					v = (v << 8) | _in[_off + i];
				return v;
			}
		}

		std::string filename(const std::string& _homePath, const MachineModel _model)
		{
			if(_homePath.empty())
				return {};
			std::string base = _homePath;
			if(!base.empty() && base.back() != '/' && base.back() != '\\')
				base += '/';
			return base + "nvram/"
				+ (_model == MachineModel::Monomachine ? "mm" : "md")
				+ "-plusdrive-v1.bin";
		}

		// Config block trailer written after the sparse sector entries. Files
		// without one load with an erased config (first boot = prep path).
		static constexpr size_t kTrailerSize = g_configSize;
		static constexpr size_t kEntrySize = 4 + g_sectorSize;

		// A corrupt count must not make the emulator allocate an arbitrary
		// amount of host memory. Real +Drive images are sparse and far below
		// this cap; an oversized file is rejected without being overwritten.
		constexpr size_t kMaxStoreFileSize = 256u * 1024u * 1024u;
		constexpr size_t kMaxSerializedPages =
			(kMaxStoreFileSize - g_headerSize - kTrailerSize) / kEntrySize;

		enum class LoadOutcome : uint8_t
		{
			Loaded,
			NotFound,
			Invalid,
			ReadError
		};

		void setError(std::string* _error, std::string _message)
		{
			if(_error)
				*_error = std::move(_message);
		}

		bool ensureParentDirectory(const std::string& _path, std::string* _error)
		{
			const auto slash = _path.find_last_of("/\\");
			if(slash == std::string::npos || slash == 0)
				return true;
			const std::string parent = _path.substr(0, slash);
			if(baseLib::filesystem::isDirectory(parent))
				return true;
			(void)baseLib::filesystem::createDirectory(parent);
			if(baseLib::filesystem::isDirectory(parent))
				return true;
			setError(_error, "cannot create +Drive backing directory " + parent);
			return false;
		}

		bool isFutureVersion(const std::string& _path)
		{
			auto* file = baseLib::filesystem::openFile(_path, "rb");
			if(!file)
				return false;
			std::vector<uint8_t> header(g_headerSize);
			const size_t got = std::fread(header.data(), 1, header.size(), file);
			std::fclose(file);
			if(got != header.size())
				return false;
			return std::memcmp(header.data(), g_magic, sizeof(g_magic)) == 0
				&& readU16(header, 10) > g_formatVersion;
		}

		bool preserveExact(const std::string& _path, const std::string& _backup,
			std::string* _error)
		{
			if(baseLib::filesystem::getFileSize(_path) > kMaxStoreFileSize)
			{
				setError(_error, "unrecognized +Drive file is too large to preserve safely");
				return false;
			}
			std::vector<uint8_t> raw;
			if(!baseLib::filesystem::readFile(raw, _path))
			{
				setError(_error, "cannot read unrecognized +Drive file before preserving it");
				return false;
			}
			if(!baseLib::filesystem::writeFileExclusive(_backup, raw))
			{
				setError(_error, "cannot create non-destructive +Drive backup " + _backup);
				return false;
			}
			return true;
		}

		LoadOutcome loadInternal(Pages& _pages, const std::string& _path,
			std::vector<uint8_t>* _config, std::string* _error)
		{
			if(_error)
				_error->clear();
			const bool loadLog = std::getenv("GEARMULATOR_PLUSDRIVE_LOADLOG") != nullptr;
			auto reject = [&](const char* _why, std::string _message)
			{
				if(loadLog)
					std::fprintf(stderr, "[LOAD] reject %s path=%s\n", _why, _path.c_str());
				setError(_error, std::move(_message));
				return LoadOutcome::Invalid;
			};
			if(_path.empty())
				return reject("empty-path", "empty +Drive backing path");
			auto* probe = baseLib::filesystem::openFile(_path, "rb");
			if(!probe)
			{
				if(baseLib::filesystem::exists(_path))
				{
					setError(_error, "cannot open +Drive backing file");
					return LoadOutcome::ReadError;
				}
				setError(_error, "+Drive backing file does not exist");
				return LoadOutcome::NotFound;
			}
			std::fclose(probe);
			if(baseLib::filesystem::isDirectory(_path))
			{
				setError(_error, "+Drive backing path is a directory");
				return LoadOutcome::ReadError;
			}
			const size_t fileSize = baseLib::filesystem::getFileSize(_path);
			if(fileSize > kMaxStoreFileSize)
				return reject("file-too-large", "+Drive backing file exceeds the safety limit");
			if(fileSize < g_headerSize)
				return reject("short-header", "truncated +Drive header");

			std::vector<uint8_t> raw;
			if(!baseLib::filesystem::readFile(raw, _path))
			{
				setError(_error, "cannot read complete +Drive backing file");
				return LoadOutcome::ReadError;
			}
			if(raw.size() != fileSize)
			{
				setError(_error, "+Drive backing file changed while it was read");
				return LoadOutcome::ReadError;
			}
			if(std::memcmp(raw.data(), g_magic, sizeof(g_magic)) != 0)
				return reject("magic", "unrecognized +Drive backing file");
			if(readU16(raw, 10) != g_formatVersion)
				return reject("version", "unsupported +Drive backing format version");
			const uint32_t count = readU32(raw, 12);
			if(count > g_sectorCount)
				return reject("count-range", "+Drive sector count is out of range");
			const uint64_t entries = static_cast<uint64_t>(count) * kEntrySize;
			const uint64_t withoutTrailer = g_headerSize + entries;
			const uint64_t withTrailer = withoutTrailer + kTrailerSize;
			if(raw.size() != withoutTrailer && raw.size() != withTrailer)
				return reject("size", "+Drive backing file size does not match its header");

			Pages pages;
			std::vector<uint8_t> config(g_configSize, 0xff);
			size_t off = g_headerSize;
			for(uint32_t i = 0; i < count; ++i, off += kEntrySize)
			{
				const uint32_t sector = readU32(raw, off);
				if(sector >= g_sectorCount)
					return reject("sector-range", "+Drive sector address is out of range");
				Page page;
				std::memcpy(page.data(), raw.data() + off + 4, g_sectorSize);
				if(!pages.emplace(sector, page).second)
					return reject("duplicate-sector", "+Drive backing file contains a duplicate sector");
			}
			if(raw.size() == withTrailer)
				std::memcpy(config.data(), raw.data() + off, kTrailerSize);
			_pages = std::move(pages);
			if(_config)
				*_config = std::move(config);
			return LoadOutcome::Loaded;
		}

		bool load(Pages& _pages, const std::string& _path,
			std::vector<uint8_t>* _config, std::string* _error)
		{
			return loadInternal(_pages, _path, _config, _error) == LoadOutcome::Loaded;
		}

		bool save(const std::string& _path, const Pages& _pages,
			const std::vector<uint8_t>& _config, std::string* _error)
		{
			if(_error)
				_error->clear();
			if(_path.empty())
			{
				setError(_error, "empty +Drive backing path");
				return false;
			}
			if(_pages.size() > g_sectorCount || _pages.size() > kMaxSerializedPages)
			{
				setError(_error, "too many +Drive sectors to persist safely");
				return false;
			}
			if(!_config.empty() && _config.size() != kTrailerSize)
			{
				setError(_error, "+Drive config must be empty or exactly 16 bytes");
				return false;
			}
			for(const auto& entry : _pages)
			{
				if(entry.first >= g_sectorCount)
				{
					setError(_error, "cannot persist an out-of-range +Drive sector");
					return false;
				}
			}
			if(!ensureParentDirectory(_path, _error))
				return false;

			try
			{
				std::vector<uint8_t> raw;
				raw.reserve(g_headerSize + _pages.size() * kEntrySize + kTrailerSize);
				raw.insert(raw.end(), g_magic, g_magic + sizeof(g_magic));
				appendU16(raw, static_cast<uint16_t>(g_formatVersion));
				appendU32(raw, static_cast<uint32_t>(_pages.size()));
				for(const auto& entry : _pages)
				{
					appendU32(raw, entry.first);
					raw.insert(raw.end(), entry.second.begin(), entry.second.end());
				}
				std::vector<uint8_t> config = _config;
				config.resize(kTrailerSize, 0xff);
				raw.insert(raw.end(), config.begin(), config.end());
				if(!baseLib::filesystem::writeFileAtomic(_path, raw))
				{
					setError(_error, "cannot atomically save +Drive backing file " + _path);
					return false;
				}
				return true;
			}
			catch(const std::exception& e)
			{
				setError(_error, std::string("cannot serialize +Drive backing file: ") + e.what());
				return false;
			}
			catch(...)
			{
				setError(_error, "cannot serialize +Drive backing file");
				return false;
			}
		}

		bool ensureReady(Pages& _pages, const std::string& _path,
			std::string* _error, std::vector<uint8_t>* _config)
		{
			if(_error)
				_error->clear();
			std::string loadError;
			const auto primaryOutcome = loadInternal(_pages, _path, _config, &loadError);
			if(primaryOutcome == LoadOutcome::Loaded)
				return true;
			if(primaryOutcome == LoadOutcome::ReadError)
			{
				setError(_error, std::move(loadError));
				return false;
			}
			if(_path.empty())
			{
				setError(_error, std::move(loadError));
				return false;
			}
			if(isFutureVersion(_path))
			{
				setError(_error, "refusing to downgrade a newer +Drive backing file");
				return false;
			}

			const bool primaryExists = primaryOutcome != LoadOutcome::NotFound;
			const std::string backup = _path + ".bak";
			const bool backupExists = baseLib::filesystem::exists(backup);
			Pages recoveredPages;
			std::vector<uint8_t> recoveredConfig(g_configSize, 0xff);
			std::string backupError;
			const auto backupOutcome = loadInternal(recoveredPages, backup,
				&recoveredConfig, &backupError);
			const bool recovered = backupOutcome == LoadOutcome::Loaded;

			if(backupExists && backupOutcome == LoadOutcome::ReadError)
			{
				setError(_error, "cannot read existing +Drive backup: " + backupError);
				return false;
			}
			if(!primaryExists && recovered)
			{
				std::string saveError;
				if(!save(_path, recoveredPages, recoveredConfig, &saveError))
				{
					setError(_error, std::move(saveError));
					return false;
				}
				_pages = std::move(recoveredPages);
				if(_config)
					*_config = std::move(recoveredConfig);
				return true;
			}
			if(primaryExists && backupExists)
			{
				setError(_error, "unrecognized +Drive file and an existing backup are both present; preserve both before retrying");
				return false;
			}
			if(!primaryExists && backupExists)
			{
				setError(_error, "missing +Drive file and its existing backup is invalid: " + backupError);
				return false;
			}
			if(primaryExists && !preserveExact(_path, backup, _error))
				return false;

			Pages freshPages;
			std::vector<uint8_t> freshConfig(g_configSize, 0xff);
			std::string saveError;
			if(!save(_path, freshPages, freshConfig, &saveError))
			{
				setError(_error, std::move(saveError));
				return false;
			}
			_pages = std::move(freshPages);
			if(_config)
				*_config = std::move(freshConfig);
			return true;
		}
	}
}
