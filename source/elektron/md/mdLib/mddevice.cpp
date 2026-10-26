#include "mddevice.h"

#include "mdosvariant.h"
#include "mdstate.h"
#include "mdromloader.h"
#include "mdtypes.h"

	#include "baseLib/filesystem.h"
	#include "synthLib/realtimeInstrumentation.h"

	#include <algorithm>
	#include <atomic>
	#include <cstdio>

namespace
{
	std::atomic<uint64_t> g_sysexDeviceIds{0};
	std::vector<uint8_t> loadInitialPatchRam(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model, const std::vector<uint8_t>& _initialPatchRam)
	{
		if(!_initialPatchRam.empty())
			return _initialPatchRam;
		if(_model != md::MachineModel::Monomachine || _params.homePath.empty())
			return {};

		const auto filename = baseLib::filesystem::validatePath(_params.homePath)
			+ "nvram/mm-factory-live3-be.bin";
		std::vector<uint8_t> data;
		if(!baseLib::filesystem::readFile(data, filename))
			return {};

		if(data.size() != md::g_patchRamStateSize)
		{
			std::fprintf(stderr,
				"[MM] ignoring factory patch RAM with unexpected size: %s (%zu bytes, expected %u)\n",
				filename.c_str(), data.size(), md::g_patchRamStateSize);
			return {};
		}

		std::fprintf(stderr, "[MM] factory patch RAM discovered at %s (%zu bytes)\n",
			filename.c_str(), data.size());
		return data;
	}

	std::string mdFlashCacheFilename(const std::string& _homePath,
		const md::MachineModel _model)
	{
		if(_model != md::MachineModel::Machinedrum || _homePath.empty())
			return {};
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/md-uw-1.63-factory-v2.cache";
	}

	std::string mdFlashCacheFilename(const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model)
	{
		return mdFlashCacheFilename(_params.homePath, _model);
	}

	struct InitialMdFlash
	{
		std::vector<uint8_t> flash;
		std::vector<uint8_t> cache;
		// Machine-local patch RAM (MD kits, patterns and songs live there):
		// loaded from the machine's user-RAM sidecar when present, else
		// empty for the historical factory boot.
		std::vector<uint8_t> patchRam;
	};

	InitialMdFlash loadInitialMdFlash(
		const synthLib::DeviceCreateParams& _params, const md::MachineModel _model)
	{
		const auto filename = mdFlashCacheFilename(_params, _model);
		std::vector<uint8_t> cache;
		if(filename.empty() || !baseLib::filesystem::readFile(cache, filename))
			return {};

		md::Rom rom;
		if(!_params.romData.empty())
		{
			md::Rom supplied(_params.romData, _params.romName);
			if(supplied.isValid() && md::RomLoader::isRomForModel(
				supplied.data(), _model))
				rom = std::move(supplied);
		}
		if(!rom.isValid())
			rom = md::RomLoader::findROM(_model);

		InitialMdFlash result;
		if(!rom.isValid()
			|| !md::decodeFactoryFlashCache(result.flash, cache, rom.data()))
		{
			std::fprintf(stderr,
				"[MD] ignoring invalid or ROM-mismatched UW factory cache: %s\n",
				filename.c_str());
			return {};
		}
		result.cache = std::move(cache);
		return result;
	}

	InitialMdFlash loadInitialMdFlash(const md::Rom& _rom,
		const std::string& _filename,
		const std::vector<uint8_t>* _baselineImage = nullptr)
	{
		InitialMdFlash result;
		// The cache validates against the image it was encoded from: the
		// stock ROM, or a registered variant image for variant caches.
		const auto& baseline = _baselineImage ? *_baselineImage : _rom.data();
		if(!_rom.isValid() || _filename.empty() || baseline.size() != md::g_romSize
			|| !baseLib::filesystem::readFile(result.cache, _filename)
			|| !md::decodeFactoryFlashCache(result.flash, result.cache, baseline))
			return {};
		return result;
	}

	md::Rom loadStateRom(const std::vector<uint8_t>& _romData,
		const std::string& _romName,
		const md::MachineModel _model)
	{
		if(!_romData.empty())
		{
			md::Rom rom(_romData, _romName);
			if(rom.isValid() && md::RomLoader::isRomForModel(rom.data(), _model))
				return rom;
		}
		return md::RomLoader::findROM(_model);
	}

	// Upgraded OS image side file. Lives outside project state like real
	// flash, keyed to its source ROM so a ROM swap ignores it. Delete the
	// file to return to the stock ROM image.
	constexpr uint32_t g_osUpgradeRegionBegin = 0x00004000;
	constexpr uint32_t g_osUpgradeRegionEnd = 0x00200000;

	std::string mdOsUpgradeFilename(const std::string& _homePath,
		const md::MachineModel _model, const uint64_t _romFingerprint)
	{
		if(_homePath.empty())
			return {};
		char suffix[20];
		std::snprintf(suffix, sizeof(suffix), "%016llx",
			static_cast<unsigned long long>(_romFingerprint));
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/" + (_model == md::MachineModel::Monomachine ? "mm" : "md")
			+ "-os-upgrade-" + suffix + ".bin";
	}

	std::vector<uint8_t> loadInitialOsImage(const std::vector<uint8_t>& _romData,
		const std::string& _romName, const md::MachineModel _model,
		const std::string& _homePath)
	{
		const auto rom = loadStateRom(_romData, _romName, _model);
		if(!rom.isValid() || _homePath.empty())
			return {};
		const auto filename = mdOsUpgradeFilename(_homePath, _model,
			md::fingerprint(rom.data()));
		std::vector<uint8_t> image;
		if(filename.empty() || !baseLib::filesystem::readFile(image, filename)
			|| image.size() != rom.data().size())
			return {};
		std::fprintf(stderr, "[%s] upgraded OS image discovered at %s (%zu bytes)\n",
			_model == md::MachineModel::Monomachine ? "MM" : "MD",
			filename.c_str(), image.size());
		return image;
	}

	// Legacy boot resolution shared by fresh construction and live "back to
	// default" reboots: validated factory cache first, then a previously
	// MIDI-upgraded OS image persisted for this ROM, stock ROM bytes
	// otherwise (empty flash boots the ROM image directly).
	InitialMdFlash resolveLegacyInitialFlash(
		const synthLib::DeviceCreateParams& _params,
		const md::MachineModel _model)
	{
		// Freshest first: the last hibernated live image, then the
		// settled factory cache, then a persisted MIDI upgrade, stock
		// ROM bytes otherwise (empty flash boots the ROM directly).
		// Hibernation is OS-region-validated on load, so it can never
		// mask a foreign image; a completed MIDI upgrade deletes the
		// hibernation files so the upgrade always wins.
		if(!_params.homePath.empty())
		{
			const auto rom = loadStateRom(_params.romData, _params.romName,
				_model);
			if(rom.isValid())
			{
				std::vector<uint8_t> hibernated;
				if(md::loadOsHibernation(hibernated, _params.homePath,
					_model, 0, rom.data()))
				{
					InitialMdFlash hibernation;
					hibernation.flash = std::move(hibernated);
					// The settled cache rides along so the machine does
					// not look uninitialized (which would needlessly
					// re-run factory capture and its reboot).
					auto settled = loadInitialMdFlash(rom,
						mdFlashCacheFilename(_params.homePath, _model));
					hibernation.cache = std::move(settled.cache);
					md::loadUserPatchRam(hibernation.patchRam, _params.homePath,
						_model);
					std::fprintf(stderr, "[MD] default boot: hibernation%s%s\n",
						hibernation.cache.empty() ? " (no cache)" : " (+cache)",
						hibernation.patchRam.empty() ? "" : " (+patchram)");
					return hibernation;
				}
			}
		}
		auto initialFlash = loadInitialMdFlash(_params, _model);
		if(initialFlash.flash.empty())
		{
			initialFlash.flash = loadInitialOsImage(_params.romData,
				_params.romName, _model, _params.homePath);
		}
		if(initialFlash.patchRam.empty())
			md::loadUserPatchRam(initialFlash.patchRam, _params.homePath,
				_model);
		std::fprintf(stderr, "[MD] default boot: %s%s\n",
			!initialFlash.cache.empty() ? "cache"
			: !initialFlash.flash.empty() ? "image/upgrade" : "rom",
			initialFlash.patchRam.empty() ? "" : " (+patchram)");
		return initialFlash;
	}

	// Variant boot resolution: the registered image, overlaid with its
	// initialized factory baseline when one was captured before. The cache
	// is validated against the variant image itself (its fingerprint binds
	// it), so a stock cache can never boot a variant machine and a
	// variant cache can never leak into a stock boot.
	InitialMdFlash loadVariantCacheOnly(const std::string& _homePath,
		md::MachineModel _model, uint64_t _fingerprint);
	InitialMdFlash loadVariantInitialFlash(const std::string& _homePath,
		const md::MachineModel _model, const uint64_t _fingerprint)	{
		InitialMdFlash result;
		md::OsVariant variant;
		if(_fingerprint == 0 || !md::loadOsVariant(variant,
				md::variantFilename(_homePath, _model, _fingerprint), _model)
			|| variant.fingerprint != _fingerprint)
			return result;
		// Hibernation (last live state), then the settled factory cache,
		// then the raw registered image. Both files validate against the
		// variant image on load. The cache rides along with hibernation
		// so the machine does not look uninitialized (which would
		// needlessly re-run factory capture and its reboot).
		if(md::loadOsHibernation(result.flash, _homePath, _model,
			_fingerprint, variant.image))
		{
			const auto settled = loadVariantCacheOnly(_homePath, _model,
				_fingerprint);
			result.cache = settled.cache;
			md::loadUserPatchRam(result.patchRam, _homePath, _model);
			std::fprintf(stderr,
				"[MD] variant boot %016llx: hibernation%s%s\n",
				(unsigned long long)_fingerprint,
				result.cache.empty() ? " (no cache)" : " (+cache)",
				result.patchRam.empty() ? "" : " (+patchram)");
			return result;
		}
		result.flash = variant.image;
		const auto cacheFile = md::variantFactoryCacheFilename(_homePath,
			_model, _fingerprint);
		std::vector<uint8_t> cache;
		// decodeFactoryFlashCache only swaps on success, so result.flash
		// still holds the raw variant image when no usable cache exists.
		// The machine-local user RAM is independent of the flash source,
		// so it applies here as well.
		if(!cacheFile.empty()
			&& baseLib::filesystem::readFile(cache, cacheFile)
			&& md::decodeFactoryFlashCache(result.flash, cache, variant.image))
			result.cache = std::move(cache);
		md::loadUserPatchRam(result.patchRam, _homePath, _model);
		std::fprintf(stderr, "[MD] variant boot %016llx: %s%s\n",
			(unsigned long long)_fingerprint,
			result.cache.empty() ? "raw image (no cache)" : "cache",
			result.patchRam.empty() ? "" : " (+patchram)");
		return result;
	}

	// Loads a registered variant image by fingerprint, validating identity.
	// Returns false when the store has no such variant (e.g. its file was
	// deleted), letting callers fail loudly or fall back to legacy paths.
	bool resolveVariantBase(md::OsVariant& _variant,
		const std::string& _homePath, const md::MachineModel _model,
		const uint64_t _fingerprint)
	{
		md::OsVariant variant;
		if(_fingerprint == 0 || !md::loadOsVariant(variant,
				md::variantFilename(_homePath, _model, _fingerprint), _model)
			|| variant.fingerprint != _fingerprint)
			return false;
		_variant = std::move(variant);
		return true;
	}

	// Loads only a variant's settled factory cache (no hibernation, no
	// raw image): the reference for what a fresh factory init of that OS
	// produces. Empty when no settled cache file exists yet.
	InitialMdFlash loadVariantCacheOnly(const std::string& _homePath,
		const md::MachineModel _model, const uint64_t _fingerprint)
	{
		InitialMdFlash result;
		md::OsVariant variant;
		if(!resolveVariantBase(variant, _homePath, _model, _fingerprint))
			return result;
		const auto cacheFile = md::variantFactoryCacheFilename(_homePath,
			_model, _fingerprint);
		std::vector<uint8_t> cache;
		if(!cacheFile.empty()
			&& baseLib::filesystem::readFile(cache, cacheFile)
			&& md::decodeFactoryFlashCache(result.flash, cache, variant.image))
			result.cache = std::move(cache);
		return result;
	}
}

namespace md
{
	// Virtual +Drive attach, on by default (hardware-normal: a first boot with
	// an empty store runs the firmware's one-time prep, the second shows
	// +DRIVE). Opt out with GEARMULATOR_MDMM_PLUSDRIVE=0. The responder lives as
	// long as the Device and is re-attached to every replacement Hardware, each
	// of which owns its Sim. Backing is machine-local, shared across OSes, and
	// never part of an OS hibernation slot. setBackingPath loads or auto-creates
	// the file, so a clean setup needs no manual image.
	Device::PreparedState::~PreparedState()
	{
		if(m_hardware)
			m_hardware->getUC().getSim().setPlusDrive(nullptr);
	}

	bool Device::attachPlusDrive(Hardware& _hardware,
		const std::string& _homePath, const MachineModel _model, std::string* _error)
	{
		if(_error)
			_error->clear();
		const char* const opt = std::getenv("GEARMULATOR_MDMM_PLUSDRIVE");
		if((opt != nullptr && opt[0] == '0') || _homePath.empty())
		{
			_hardware.getUC().getSim().setPlusDrive(nullptr);
			return true;
		}
		if(!m_plusDrive)
			m_plusDrive.emplace();
		m_plusDrive->setMode(PlusDrive::Mode::Canned);
		m_plusDrive->setServeFlash(true);
		m_plusDrive->setModel(_model);
		const std::string path = plusdrive::filename(_homePath, _model);
		std::string error;
		if(!m_plusDrive->setBackingPath(path, false, &error))
		{
			_hardware.getUC().getSim().setPlusDrive(nullptr);
			if(_error)
				*_error = error;
			std::fprintf(stderr, "[%s] +Drive unavailable: %s\n",
				_model == MachineModel::Monomachine ? "MM" : "MD", error.c_str());
			return false;
		}
		_hardware.getUC().getSim().setPlusDrive(&*m_plusDrive);
		std::fprintf(stderr, "[%s] +Drive attached: %s\n",
			_model == MachineModel::Monomachine ? "MM" : "MD", path.c_str());
		return true;
	}

	bool Device::attachPreparedPlusDrive(PreparedState& _prepared, std::string* _error)
	{
		if(_error)
			_error->clear();
		if(!_prepared.m_hardware || !_prepared.m_context)
		{
			if(_error)
				*_error = "prepared Hardware has no +Drive attachment context";
			return false;
		}
		const char* const opt = std::getenv("GEARMULATOR_MDMM_PLUSDRIVE");
		if((opt != nullptr && opt[0] == '0')
			|| _prepared.m_context->m_homePath.empty())
			return true;
		if(!_prepared.m_plusDrive)
			_prepared.m_plusDrive.emplace();
		auto& drive = *_prepared.m_plusDrive;
		drive.setMode(PlusDrive::Mode::Canned);
		drive.setServeFlash(true);
		drive.setModel(_prepared.m_context->m_model);
		std::string error;
		if(!drive.setBackingPath(plusdrive::filename(
			_prepared.m_context->m_homePath, _prepared.m_context->m_model), true, &error))
		{
			if(_error)
				*_error = error;
			return false;
		}
		_prepared.m_hardware->getUC().getSim().setPlusDrive(&drive);
		return true;
	}
}	// namespace md

namespace md
{
	Device::Device(const synthLib::DeviceCreateParams& _params,
		const std::vector<uint8_t>& _initialPatchRam)
		: synthLib::Device(_params)
		, m_model(machineModelFromDeviceCustomData(_params.customData))
		, m_frontPanelPublisher(std::make_shared<FrontPanelPublisher>())
		, m_preparationContext(new PreparationContext(_params, m_model))
		, m_mdFlashCacheFilename(mdFlashCacheFilename(_params, m_model))
		, m_sysexDeviceId(g_sysexDeviceIds.fetch_add(1, std::memory_order_relaxed) + 1)
	{
		auto initialFlash = InitialMdFlash{};
		// An explicitly selected OS variant always wins: it names the exact image to
		// boot, replacing the legacy cache/upgrade lookup. A captured variant
		// factory baseline boots in place of the raw image when one exists, so init
		// does not repeat. Anything else keeps the historical behaviour unchanged, and
		// a selection that cannot be loaded fails the device visibly, like a missing
		// ROM, rather than silently booting the wrong OS.
		bool variantFailed = false;
		const bool variantBoot = _params.osVariantFingerprint != 0;
		if(variantBoot)
		{
			initialFlash = loadVariantInitialFlash(_params.homePath, m_model,
				_params.osVariantFingerprint);
			variantFailed = initialFlash.flash.empty();
		}
		if(initialFlash.flash.empty() && !variantFailed)
			initialFlash = resolveLegacyInitialFlash(_params, m_model);
		const auto romData = variantFailed ? std::vector<uint8_t>{} : _params.romData;
		const auto romName = variantFailed ? std::string{} : _params.romName;
		// Machine-local patch RAM (MD kits, patterns, songs) rides with the
		// resolved image; otherwise the legacy seed applies (MM file, ctor
		// argument).
		auto initialPatchRam = !initialFlash.patchRam.empty()
			? initialFlash.patchRam
			: loadInitialPatchRam(_params, m_model, _initialPatchRam);
		// Kits, patterns and songs are machine state, not OS content: an OS update
		// never clears a kit. The host keeps the authoritative copy and the machine
		// simply receives it, so changing OS cannot hide or lose it. The copy means a
		// firmware that reformats the store at startup costs a restore, not the
		// user's data.
		m_canonicalUserRam = initialPatchRam;
		m_hardware = std::make_unique<Hardware>(romData, romName, m_model,
			initialPatchRam, m_frontPanelPublisher,
			initialFlash.flash, initialFlash.cache, FlashSectorOverlay{}, variantBoot);
		if(!attachPlusDrive(*m_hardware, _params.homePath, m_model, &m_plusDriveError)
			&& !m_plusDriveError.empty())
			std::fprintf(stderr, "[MD] +Drive attachment unavailable: %s\n",
				m_plusDriveError.c_str());
		if(!variantFailed && variantBoot)
			m_liveVariantFingerprint = _params.osVariantFingerprint;
	}

	bool Device::userRamWasWiped() const
	{
		if(m_canonicalUserRam.empty() || !m_hardware)
			return false;
		// A store the user genuinely emptied is left alone: this only reports a
		// machine that lost a store the host still holds.
		const auto live = m_hardware->copyPatchRam();
		if(live.size() != m_canonicalUserRam.size())
			return false;
		return std::all_of(live.begin(), live.end(),
			[](const uint8_t _byte) { return _byte == 0; });
	}

	bool Device::restoreUserRamIfWiped()
	{
		if(!userRamWasWiped())
			return false;
		if(!m_hardware->getUC().replacePatchRam(m_canonicalUserRam))
			return false;
		std::fprintf(stderr,
			"[MD] the OS reformatted the kit store at startup; restored the "
			"saved kits from the host's copy\n");
		return true;
	}

	bool Device::captureFactoryFlashCachePersistence(std::string& _filename,		FactoryFlashSnapshot& _snapshot, std::string& _error)
	{
		_error.clear();
		_filename.clear();
		_snapshot = {};
		if(!m_hardware)
		{
			_error = "factory cache has no hardware to capture";
			return false;
		}
		const auto& homePath = m_preparationContext
			? m_preparationContext->m_homePath : std::string{};
		if(homePath.empty())
		{
			_error = "factory cache has no writable destination";
			return false;
		}
		if(!m_hardware->factoryFlashCacheReady())
		{
			_error = "factory cache is not ready";
			return false;
		}
		if(!m_hardware->copyFactoryFlashSnapshot(_snapshot))
		{
			_error = "validated factory cache could not be captured";
			return false;
		}
		// Route the destination by content: a baseline matching a
		// registered OS variant belongs to that variant's cache file. A
		// pre-encoded snapshot is identified the same way when it is
		// materialized below; both resolve through the same helpers.
		uint64_t baseFp = 0;
		if(!_snapshot.baseline.empty())
		{
			findOsVariantByImage(baseFp, homePath, m_model,
				_snapshot.baseline);
		}
		else if(!_snapshot.cache.empty())
		{
			auto rom = loadStateRom(m_preparationContext->m_romData,
				m_preparationContext->m_romName, m_model);
			std::vector<uint8_t> decoded;
			if(!rom.isValid()
				|| !decodeFactoryFlashCache(decoded, _snapshot.cache,
					rom.data()))
				findOsVariantByCache(baseFp, homePath, m_model, _snapshot.cache);
		}
		_snapshot.baseFingerprint = baseFp;
		_filename = baseFp != 0
			? variantFactoryCacheFilename(homePath, m_model, baseFp)
			: mdFlashCacheFilename(homePath, m_model);
		if(_filename.empty())
		{
			_error = "factory cache has no writable destination";
			return false;
		}
		return true;
	}

	bool Device::materializeFactoryFlashCache(FactoryFlashSnapshot& _snapshot,
		const std::shared_ptr<const PreparationContext>& _context,
		std::string& _error)
	{
		_error.clear();
		_snapshot.baseFingerprint = 0;
		if(!_context)
		{
			_error = "factory cache has no preparation context";
			return false;
		}
		auto rom = loadStateRom(_context->m_romData, _context->m_romName,
			_context->m_model);
		if(!rom.isValid())
		{
			_error = "validated factory cache could not be encoded";
			return false;
		}
		const auto& homePath = _context->m_homePath;
		const auto model = _context->m_model;
		if(!_snapshot.cache.empty())
		{
			// Pre-encoded snapshot: stock, or a registered variant
			// identified by trial decode. Anything else is refused rather
			// than persisted.
			std::vector<uint8_t> decoded;
			if(decodeFactoryFlashCache(decoded, _snapshot.cache, rom.data()))
				return true;
			uint64_t baseFp = 0;
			if(findOsVariantByCache(baseFp, homePath, model, _snapshot.cache))
			{
				_snapshot.baseFingerprint = baseFp;
				return true;
			}
			_error = "captured factory cache matches neither stock nor a registered OS";
			return false;
		}
		if(_snapshot.baseline.empty())
			return true;
		// The factory cache must reproduce its base image. A live image
		// carrying a foreign OS must never be persisted as a factory
		// baseline, or it would shadow other images on later boots. The
		// base is the registered variant the baseline matches, else stock.
		uint64_t baseFp = 0;
		findOsVariantByImage(baseFp, homePath, model, _snapshot.baseline);
		const std::vector<uint8_t>* baseImage = &rom.data();
		OsVariant variant;
		if(baseFp != 0)
		{
			if(!loadOsVariant(variant,
					variantFilename(homePath, model, baseFp), model)
				|| variant.fingerprint != baseFp)
			{
				_error = "registered OS variant image is missing or invalid";
				return false;
			}
			baseImage = &variant.image;
		}
		if(_snapshot.baseline.size() != g_romSize
			|| g_osUpgradeRegionEnd > _snapshot.baseline.size()
			|| !std::equal(_snapshot.baseline.begin() + g_osUpgradeRegionBegin,
				_snapshot.baseline.begin() + g_osUpgradeRegionEnd,
				baseImage->begin() + g_osUpgradeRegionBegin))
		{
			_error = baseFp != 0
				? "live flash does not match the selected OS variant, factory cache not rewritten"
				: "live flash carries an OS upgrade, factory cache not rewritten";
			return false;
		}
		if(!encodeFactoryFlashCache(_snapshot.cache,
			_snapshot.baseline, *baseImage))
		{
			_error = "validated factory cache could not be encoded";
			return false;
		}
		_snapshot.baseline.clear();
		_snapshot.baseFingerprint = baseFp;
		return true;
	}

	Device::~Device()
	{
		// Best effort, teardown thread only: file IO here can overrun audio.
		hibernateCurrentOs();
		persistOsUpgradeImage();
		// Break Sim -> responder ownership before the optional endpoints are
		// destroyed. This keeps teardown order explicit and prevents a retired
		// Hardware from observing a dangling +Drive pointer.
		if(m_hardware)
			m_hardware->getUC().getSim().setPlusDrive(nullptr);
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware)
			m_deferredPreparedState->m_hardware->getUC().getSim().setPlusDrive(nullptr);
	}

	bool Device::hibernateCurrentOs(std::string* const _error)
	{
		if(!m_hardware || !m_preparationContext
			|| m_preparationContext->m_homePath.empty())
		{
			if(_error)
				*_error = "there is no live machine to hibernate";
			return false;
		}
		if(isProjectStateRestorePending() || hasDeferredStateRestore())
		{
			if(_error)
				*_error = "the machine is restoring state";
			return false;
		}
		// Machinedrum only: the Monomachine keeps user data in patch RAM
		// (carried explicitly on switches, seeded from file on boot), not
		// in flash hibernation scope.
		if(m_model != MachineModel::Machinedrum)
		{
			if(_error)
				*_error = "hibernation is Machinedrum-only";
			return false;
		}
		// Hibernation records the last live state unconditionally: a mid-init image
		// resumes factory init on the next boot (init is progressive over flash, so a
		// partial image is strictly closer to settled than the raw image), while
		// skipping hibernation would strand saves that exist only in the live
		// machine. OS-region validation still refuses corrupt images, and the
		// settled factory cache remains the long-term baseline.
		return hibernateOsImage(m_preparationContext->m_homePath, m_model,
			m_hardware->copyFlashData(), m_preparationContext->m_romData,
			m_preparationContext->m_romName, _error,
			copyUserRamForPersistence());
	}

	bool Device::hibernateOsImage(const std::string& _homePath,
		const MachineModel _model, const std::vector<uint8_t>& _flash,
		const std::vector<uint8_t>& _romData, const std::string& _romName,
		std::string* const _error, const std::vector<uint8_t>& _patchRam)
	{
		const auto fail = [_error](const char* const _message)
		{
			if(_error)
				*_error = _message;
			return false;
		};
		if(_homePath.empty())
			return fail("there is no home path for OS hibernation");
		if(_flash.size() != g_romSize)
			return fail("there is no live flash image to hibernate");
		// Slot by content: the registered variant the live OS matches,
		// else the stock slot (writeOsHibernation validates against the
		// stock ROM there and refuses anything else). The ROM is only
		// needed for the stock slot.
		uint64_t fingerprint = 0;
		findOsVariantByImage(fingerprint, _homePath, _model, _flash);
		OsVariant variant;
		const auto rom = loadStateRom(_romData, _romName, _model);
		const std::vector<uint8_t> noRom;
		const std::vector<uint8_t>* expected =
			rom.isValid() ? &rom.data() : &noRom;
		if(fingerprint != 0)
		{
			if(!loadOsVariant(variant,
					variantFilename(_homePath, _model, fingerprint), _model)
				|| variant.fingerprint != fingerprint)
				return fail("the registered OS variant image is missing");
			expected = &variant.image;
		}
		else if(expected->empty())
			return fail("the firmware ROM is unavailable");
		if(!writeOsHibernation(_homePath, _model, fingerprint, _flash,
			*expected))
			return fail("the live image failed validation");
		// MD kits live in patch RAM: persist the machine-local copy next to
		// the flash image (a missing sidecar boots legacy patch RAM). The
		// kits are the part of the state that must not be lost silently, so
		// a failed write is reported. Monomachine patch RAM keeps its
		// existing file/seed semantics.
		if(_model == MachineModel::Machinedrum && !_patchRam.empty()
			&& !writeUserPatchRam(_homePath, _model, _patchRam))
			return fail("the live patch RAM could not be persisted");
		return true;
	}

	void Device::persistOsUpgradeImage()
	{
		if(!m_hardware || !m_preparationContext
			|| m_preparationContext->m_homePath.empty())
			return;
		const auto flash = m_hardware->copyFlashData();
		if(flash.size() != g_romSize || g_osUpgradeRegionEnd > flash.size())
			return;
		// Stock image: nothing to persist.
		const auto rom = loadStateRom(m_preparationContext->m_romData,
			m_preparationContext->m_romName, m_model);
		if(rom.isValid() && std::equal(flash.begin() + g_osUpgradeRegionBegin,
			flash.begin() + g_osUpgradeRegionEnd,
			rom.data().begin() + g_osUpgradeRegionBegin))
			return;
		// A registered variant image belongs to the variant store, not this slot:
		// persisting it here would shadow stock and any genuine upgrade on later
		// default boots. Content-based, so it holds however the image started running
		// (fresh boot, live switch, project restore). A live OS the store no longer
		// knows keeps the legacy "OS sticks" behaviour.
		uint64_t variantFingerprint = 0;
		if(findOsVariantByImage(variantFingerprint,
			m_preparationContext->m_homePath, m_model, flash))
			return;
		if(!m_hardware->flashDirty())
			return;
		if(!rom.isValid())
			return;
		const auto filename = mdOsUpgradeFilename(
			m_preparationContext->m_homePath, m_model,
			md::fingerprint(rom.data()));
		if(filename.empty())
			return;
		std::vector<uint8_t> existing;
		if(baseLib::filesystem::readFile(existing, filename) && existing == flash)
			return;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(filename));
		const char* const tag = m_model == MachineModel::Monomachine ? "MM" : "MD";
		const bool written = existing.empty()
			? baseLib::filesystem::writeFileExclusive(filename, flash)
			: baseLib::filesystem::writeFileAtomic(filename, flash);
		if(written)
		{
			std::fprintf(stderr, "[%s] persisted upgraded OS image (%zu bytes) to %s\n",
				tag, flash.size(), filename.c_str());
		}
		else
		{
			std::fprintf(stderr, "[%s] failed to persist upgraded OS image to %s\n",
				tag, filename.c_str());
		}
	}

	bool Device::writeFactoryFlashCachePersistence(const std::string& _filename,
		const std::vector<uint8_t>& _cache, std::string& _error)
	{
		_error.clear();
		if(_filename.empty() || _cache.empty())
		{
			_error = "factory cache persistence data is incomplete";
			return false;
		}

		// Project activity can never update a valid cache. Invalid or ROM-mismatched
		// data is replaced atomically so one damaged cache cannot poison every boot.
		// This method performs only filesystem work and is called outside the device
		// lock by the processor's first-run service.
		std::vector<uint8_t> existing;
		const bool exists = baseLib::filesystem::readFile(existing, _filename);
		if(exists && existing == _cache)
			return true;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(_filename));
		const bool written = exists
			? baseLib::filesystem::writeFileAtomic(_filename, _cache)
			: baseLib::filesystem::writeFileExclusive(_filename, _cache);
		if(written)
		{
			std::fprintf(stderr, "[MD] stored UW factory cache: %s\n",
				_filename.c_str());
			return true;
		}
		// A concurrent first-run instance may have won the exclusive create.
		existing.clear();
		if(baseLib::filesystem::readFile(existing, _filename) && existing == _cache)
			return true;
		_error = "could not store UW factory cache at " + _filename;
		return false;
	}

	float Device::getSamplerate() const
	{
		return g_samplerate;
	}

	bool Device::isValid() const
	{
		return m_hardware->isValid();
	}

	bool Device::getState(std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		if(isProjectStateRestorePending() && _type == m_requestedStateType
			&& m_requestedState)
		{
			_state.insert(_state.end(), m_requestedState->begin(), m_requestedState->end());
			return true;
		}

		auto* stateHardware = m_hardware.get();
		uint64_t stateFp = m_liveVariantFingerprint;
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware)
		{
			stateHardware = m_deferredPreparedState->m_hardware.get();
			stateFp = m_deferredPreparedState->m_osVariantFingerprint;
		}
		// While an initialization is pending, the stored user RAM is the
		// machine's real user data: the live kit store is the factory reset
		// the initialization just wrote, and encoding that would lose the
		// user's kits in every project that is saved before it settles.
		const auto patchRam = !m_preservedUserRam.empty()
			? m_preservedUserRam : stateHardware->copyPatchRam();
		if(m_model == MachineModel::Monomachine)
			return encodeState(_state, patchRam, m_model, _type,
				stateHardware->copyUserFlash());
		const auto& homePath = m_preparationContext
			? m_preparationContext->m_homePath : std::string{};
		// A variant-running machine encodes against its registered image
		// so restores validate; anything else keeps the legacy stock
		// behavior bit-identical (including the fallback when the
		// variant file went missing mid-session).
		OsVariant stateVariant;
		if(stateFp != 0 && !homePath.empty()
			&& resolveVariantBase(stateVariant, homePath, m_model, stateFp))
		{
			FactoryFlashSnapshot snapshot;
			std::vector<uint8_t> baseline;
			if(stateHardware->copyFactoryFlashSnapshot(snapshot))
			{
				if(!snapshot.cache.empty())
					decodeFactoryFlashCache(baseline, snapshot.cache,
						stateVariant.image);
				else if(!snapshot.baseline.empty())
					baseline = snapshot.baseline;
			}
			const auto osMatches = baseline.size() == g_romSize
				&& std::equal(baseline.begin() + g_variantBaseAddress,
					baseline.begin() + g_variantSplitAddress,
					stateVariant.image.begin() + g_variantBaseAddress);
			if(osMatches)
				return encodeStateWithFactoryBaseline(_state, patchRam,
					stateHardware->copyFlashData(),
					baseline, stateVariant.image, m_model, _type);
			return encodeState(_state, patchRam, stateHardware->copyFlashData(),
				stateVariant.image, stateVariant.image, m_model, _type);
		}
		std::vector<uint8_t> factoryBaseline;
		if(stateHardware->copyFactoryFlashBaseline(factoryBaseline))
			return encodeStateWithFactoryBaseline(_state, patchRam,
				stateHardware->copyFlashData(),
				factoryBaseline, stateHardware->flashBaseline(), m_model, _type);
		FlashSectorOverlay pending;
		if(stateHardware->copyPendingFlashOverlay(pending))
			return encodeState(_state, patchRam, pending,
				stateHardware->flashBaseline(), m_model, _type);
		// If interaction happened before the first machine-local baseline was
		// captured, preserve a complete flash image. An absolute sector set records
		// ROM-equal deletions and lets the replacement boot coherently without waiting
		// for another factory-initialization pass.
		return encodeState(_state, patchRam, stateHardware->copyFlashData(),
			stateHardware->flashBaseline(), stateHardware->flashBaseline(), m_model, _type);
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		auto transaction = beginStateTransaction(
			std::make_shared<const std::vector<uint8_t>>(_state), _type);
		if(!transaction)
			return false;
		const auto preparationSucceeded = transaction->prepare();
		return finishStateTransaction(*transaction) && preparationSucceeded;
	}

	bool Device::StateTransactionImpl::prepare()
	{
		// Release an interrupted candidate before constructing the replacement. Both
		// operations can tear down or create a complete emulated machine.
		m_displaced.reset();
		m_prepared = m_state
			? Device::prepareState(m_context, *m_state, m_type,
				m_factoryFlash, &m_error) : nullptr;
		if(!m_state)
			m_error = "The project state payload is missing.";
		return m_prepared != nullptr;
	}

	std::unique_ptr<synthLib::Device::StateTransaction> Device::beginStateTransaction(
		std::shared_ptr<const std::vector<uint8_t>> _state,
		const synthLib::StateType _type)
	{
		if(!_state)
			return {};
		FactoryFlashSnapshot factoryFlash;
		if(m_model == MachineModel::Machinedrum)
			(void)m_hardware->copyFactoryFlashSnapshot(factoryFlash);
		auto displaced = std::move(m_deferredPreparedState);
		++m_deferredStateGeneration;
		m_requestedState = _state;
		m_requestedStateType = _type;
		m_restoreStatus = ProjectStateRestoreStatus::Preparing;
		m_restoreError.clear();
		auto transaction = std::unique_ptr<StateTransactionImpl>(
			new StateTransactionImpl(m_preparationContext, std::move(_state), _type,
				std::move(factoryFlash), m_deferredStateGeneration,
				std::move(displaced)));
		// A state-load attempt invalidates confirmations and interrupts an import,
		// even if preparing that state later fails. Never continue writing an old
		// file into a newly selected project. The UART cancellation drains normally.
		(void)m_hardware->cancelMidiSysexTransfer(transaction->m_retiredSysex);
		return transaction;
	}

	bool Device::finishStateTransaction(synthLib::Device::StateTransaction& _transaction)
	{
		auto* const transaction = dynamic_cast<StateTransactionImpl*>(&_transaction);
		if(!transaction || transaction->m_context != m_preparationContext
			|| transaction->m_generation != m_deferredStateGeneration)
			return false;
		if(!transaction->m_prepared)
		{
			failProjectStateRestore(transaction->m_error.empty()
				? "The project state could not be prepared."
				: transaction->m_error);
			return false;
		}
		transaction->m_prepared->m_hardware->requestRamRecordingMode(m_ramRecordingMode);
		if(transaction->m_prepared->m_hardware->isProjectStateRestorePending())
		{
			m_deferredPreparedState = std::move(transaction->m_prepared);
			m_restoreStatus = ProjectStateRestoreStatus::Initializing;
			return true;
		}
		if(!commitPreparedState(*transaction->m_prepared))
		{
			failProjectStateRestore("The prepared project state could not be committed.");
			return false;
		}
		clearProjectStateRestore();
		return true;
	}

	std::unique_ptr<Device::PreparedState> Device::takeFinishedDeferredState(
		uint64_t& _generation)
	{
		if(!m_deferredPreparedState || !m_deferredPreparedState->m_hardware
			|| m_restoreStatus != ProjectStateRestoreStatus::Initializing
			|| m_deferredPreparedState->m_hardware->isProjectStateRestorePending())
			return {};
		_generation = m_deferredStateGeneration;
		m_restoreStatus = ProjectStateRestoreStatus::Finalizing;
		return std::move(m_deferredPreparedState);
	}

	std::unique_ptr<Device::PreparedState> Device::makeDeferredStateReboot(
		PreparedState& _validated)
	{
		if(!_validated.m_context || !_validated.m_hardware
			|| !_validated.m_hardware->isValid()
			|| _validated.m_hardware->isProjectStateRestorePending())
			return {};
		const auto cache = _validated.m_hardware->copyFactoryFlashCache();
		if(cache.empty())
			return {};
		auto replacement = std::make_unique<Hardware>(
			_validated.m_context->m_romData, _validated.m_context->m_romName,
			_validated.m_context->m_model, _validated.m_hardware->copyPatchRam(),
			std::shared_ptr<FrontPanelPublisher>{},
			_validated.m_hardware->copyFlashData(), cache);
		if(!replacement->isValid())
			return {};
		auto reboot = std::unique_ptr<PreparedState>(new PreparedState(
			_validated.m_context, std::move(replacement), true));
		reboot->m_osVariantFingerprint = _validated.m_osVariantFingerprint;

		// Carry the validated candidate's speculative store, rather than
		// forking a fresh overlay from the primary. The candidate may have
		// formatted +Drive while its deferred project state initialized.
		if(_validated.m_plusDrive)
		{
			_validated.m_hardware->getUC().getSim().setPlusDrive(nullptr);
			auto store = _validated.m_plusDrive->takeStoreForReboot();
			reboot->m_plusDrive.emplace();
			auto& drive = *reboot->m_plusDrive;
			drive.setMode(PlusDrive::Mode::Canned);
			drive.setServeFlash(true);
			drive.setModel(_validated.m_context->m_model);
			drive.adoptStore(std::move(store));
			drive.resetTransaction();
			reboot->m_hardware->getUC().getSim().setPlusDrive(&drive);
		}
		else
		{
			std::string error;
			if(!attachPreparedPlusDrive(*reboot, &error))
				return {};
		}
		return reboot;
	}

	std::unique_ptr<Device::PreparedState> Device::prepareOsVariantBoot(
		std::shared_ptr<const PreparationContext> _context,
		const std::vector<uint8_t>& _liveFlash,
		const std::vector<uint8_t>& _livePatchRam,
		const uint64_t _fingerprint, const std::vector<uint8_t>& _userSectors,
		std::string* const _error)
	{
		const auto fail = [_error](const char* const _message)
		{
			if(_error)
				*_error = _message;
			return std::unique_ptr<PreparedState>{};
		};
		if(_error)
			_error->clear();
		if(!_context)
			return fail("There is no live machine to switch OS from.");
		const auto model = _context->m_model;
		const auto& homePath = _context->m_homePath;
		if(homePath.empty())
			return fail("The device has no home path for OS variants.");
		// The user-space split below the Machinedrum layout is unverified
		// for the Monomachine: refuse carried sectors there rather than
		// splice at a guess. Whole-image variant boots stay available.
		if(model != MachineModel::Machinedrum && !_userSectors.empty())
			return fail("Carried user sectors are not supported for this model.");
		auto base = loadVariantInitialFlash(homePath, model, _fingerprint);
		if(base.flash.empty())
			return fail("The selected OS variant is unknown or invalid.");
		std::vector<uint8_t> flash = std::move(base.flash);
		if(model == MachineModel::Machinedrum)
		{
			// Live sectors carry over the settled base only when they belong to the
			// target OS (same fingerprint). User sectors are OS-layout-specific (X.13
			// +Drive vs stock 1.63) and the target firmware factory-resets foreign
			// layouts on boot, so carrying them would trigger that reset and destroy
			// stored state; booting the target's own hibernation instead keeps every OS
			// on its own last state. Within one OS, a live image with no user
			// modifications (factory-reset or erased flash) still must not overwrite a
			// stored base holding saves. Explicit sector seeds always apply as given.
			constexpr size_t userSize = g_romSize - g_variantSplitAddress;
			const auto userErased = [](const std::vector<uint8_t>& _image)
			{
				return _image.size() == g_romSize
					&& std::all_of(_image.begin() + g_variantSplitAddress,
						_image.end(), [](const uint8_t _byte) { return _byte == 0xff; });
			};
			// Factory baseline user sectors for an OS fingerprint (0 is
			// stock): empty when no settled cache file exists yet.
			const auto factoryUser = [&](const uint64_t _fp)
			{
				const InitialMdFlash settled = _fp == 0
					? loadInitialMdFlash(loadStateRom(_context->m_romData,
						_context->m_romName, model),
						mdFlashCacheFilename(homePath, model))
					: loadVariantCacheOnly(homePath, model, _fp);
				if(settled.flash.size() != g_romSize)
					return std::vector<uint8_t>{};
				return std::vector<uint8_t>(
					settled.flash.begin() + g_variantSplitAddress,
					settled.flash.end());
			};
			// True when the user range holds saves: not erased, and
			// different from what a fresh factory init produces (when a
			// baseline is available to compare against).
			const auto userHasData = [&](const std::vector<uint8_t>& _image,
				const std::vector<uint8_t>& _baselineUser)
			{
				if(_image.size() != g_romSize || userErased(_image))
					return false;
				if(_baselineUser.empty())
					return true;
				return !std::equal(_image.begin() + g_variantSplitAddress,
					_image.end(), _baselineUser.begin());
			};
			const char* liveState = "modified";
			const char* storedState = "modified";
			bool carryLive = true;
			if(_userSectors.empty())
			{
				if(_liveFlash.size() != g_romSize)
					return fail("The live machine has no flash image to carry over.");
				uint64_t liveFp = 0;
				findOsVariantByImage(liveFp, homePath, model, _liveFlash);
				const auto liveBaseline = factoryUser(liveFp);
				const auto storedBaseline = factoryUser(_fingerprint);
				const bool sameOs = liveFp == _fingerprint;
				const bool liveData = sameOs
					&& userHasData(_liveFlash, liveBaseline);
				const bool storedData = userHasData(flash, storedBaseline);
				liveState = !sameOs ? "foreign"
					: userErased(_liveFlash) ? "erased"
					: (!liveBaseline.empty()
						&& std::equal(_liveFlash.begin() + g_variantSplitAddress,
							_liveFlash.end(), liveBaseline.begin()))
					? "factory" : "modified";
				storedState = userErased(flash) ? "erased"
					: (!storedBaseline.empty()
						&& std::equal(flash.begin() + g_variantSplitAddress,
							flash.end(), storedBaseline.begin()))
					? "factory" : "modified";
				carryLive = liveData || !storedData;
				if(carryLive)
					std::copy(_liveFlash.begin() + g_variantSplitAddress,
						_liveFlash.end(), flash.begin() + g_variantSplitAddress);
			}
			else
			{
				if(_userSectors.size() != userSize)
					return fail("The user sectors have the wrong size.");
				std::copy(_userSectors.begin(), _userSectors.end(),
					flash.begin() + g_variantSplitAddress);
			}
			std::fprintf(stderr,
				"[MD] OS switch %016llx: live user sectors %s, stored %s, %s\n",
				(unsigned long long)_fingerprint, liveState, storedState,
				_userSectors.empty()
					? (carryLive ? "carrying live" : "keeping stored")
					: "explicit seed");
		}
		// MD kits, patterns and songs live in patch RAM, battery-backed machine
		// state rather than OS content: an OS update never clears a kit. The live RAM
		// therefore always rides across, so the selected OS shows the same kits and
		// switching back never resurrects an older snapshot. The stored snapshot only
		// seeds a boot with no live machine. The Monomachine keeps its explicit seed
		// semantics (factory file or carried RAM).
		std::vector<uint8_t> initialPatchRam = model == MachineModel::Monomachine
			|| !_livePatchRam.empty() ? _livePatchRam : std::move(base.patchRam);
		std::fprintf(stderr, "[MD] OS switch %016llx: patch RAM %s (%zu bytes)\n",
			(unsigned long long)_fingerprint,
			model == MachineModel::Monomachine ? "seed"
				: !_livePatchRam.empty() ? "carried live" : "stored snapshot",
			initialPatchRam.size());
		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName,
			model,
			std::move(initialPatchRam),
			std::shared_ptr<FrontPanelPublisher>{},
			std::move(flash), std::move(base.cache), FlashSectorOverlay{}, true);
		if(!replacement->isValid())
			return fail("The replacement machine rejected the OS variant image.");
		auto prepared = std::unique_ptr<PreparedState>(
			new PreparedState(_context, std::move(replacement), true));
		std::string error;
		if(!attachPreparedPlusDrive(*prepared, &error))
			return fail(error.c_str());
		prepared->m_osVariantFingerprint = _fingerprint;
		// A target OS that has never run initializes its own flash on this
		// Kits are machine state, not OS content: the live user RAM rides across
		// to the new OS untouched, and the host keeps its own copy regardless.
		return prepared;
	}

	std::unique_ptr<Device::PreparedState> Device::prepareOsVariantBoot(
		const uint64_t _fingerprint, const std::vector<uint8_t>& _userSectors,
		std::string* const _error)
	{
		if(!m_preparationContext || !m_hardware || !m_hardware->isValid())
		{
			if(_error)
				*_error = "There is no live machine to switch OS from.";
			return {};
		}
		return prepareOsVariantBoot(m_preparationContext,
			m_hardware->copyFlashData(), m_hardware->copyPatchRam(),
			_fingerprint, _userSectors, _error);
	}

	std::unique_ptr<Device::PreparedState> Device::prepareDefaultBoot(
		std::shared_ptr<const PreparationContext> _context,
		const std::vector<uint8_t>& _livePatchRam,
		std::string* const _error)
	{
		const auto fail = [_error](const char* const _message)
		{
			if(_error)
				*_error = _message;
			return std::unique_ptr<PreparedState>{};
		};
		if(_error)
			_error->clear();
		if(!_context)
			return fail("There is no live machine to reboot from.");
		synthLib::DeviceCreateParams params;
		params.romData = _context->m_romData;
		params.romName = _context->m_romName;
		params.homePath = _context->m_homePath;
		params.customData = deviceCustomData(_context->m_model);
		auto initialFlash = resolveLegacyInitialFlash(params,
			_context->m_model);
		// Same machine-local RAM rule as a variant switch: the live RAM
		// (kits/patterns/songs) is carried into the default OS, and the
		// stored snapshot only seeds a boot with no live machine.
		std::vector<uint8_t> initialPatchRam =
			_context->m_model == MachineModel::Monomachine
				|| !_livePatchRam.empty() ? _livePatchRam
				: std::move(initialFlash.patchRam);
		std::fprintf(stderr, "[MD] default boot: patch RAM %s (%zu bytes)\n",
			_context->m_model == MachineModel::Monomachine ? "seed"
				: !_livePatchRam.empty() ? "carried live" : "stored snapshot",
			initialPatchRam.size());
		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName, _context->m_model,
			std::move(initialPatchRam),
			std::shared_ptr<FrontPanelPublisher>{},
			initialFlash.flash, initialFlash.cache);
		if(!replacement->isValid())
			return fail("The replacement machine rejected the default boot image.");
		auto prepared = std::unique_ptr<PreparedState>(
			new PreparedState(_context, std::move(replacement), true));
		std::string error;
		if(!attachPreparedPlusDrive(*prepared, &error))
			return fail(error.c_str());
		// Kits are machine state, not OS content: the live user RAM rides across
		// to the new OS untouched, and the host keeps its own copy regardless.
		return prepared;
	}

	std::unique_ptr<Device::PreparedState> Device::prepareDefaultBoot(
		std::string* const _error)
	{
		if(!m_preparationContext || !m_hardware || !m_hardware->isValid())
		{
			if(_error)
				*_error = "There is no live machine to reboot from.";
			return {};
		}
		return prepareDefaultBoot(m_preparationContext,
			m_hardware->copyPatchRam(), _error);
	}

	std::unique_ptr<Device::PreparedState> Device::prepareState(
		std::shared_ptr<const PreparationContext> _context,
		const std::vector<uint8_t>& _state, const synthLib::StateType _type,
		const FactoryFlashSnapshot& _factoryFlash, std::string* const _error,
		const std::optional<PanelPacket>& _bootHold,
		const std::vector<uint8_t>& _userRamOverride)
	{
		const auto fail = [_error](const char* const _message)
		{
			if(_error)
				*_error = _message;
			return std::unique_ptr<PreparedState>{};
		};
		if(_error)
			_error->clear();
		if(!_context)
			return fail("The project state has no preparation context.");

		std::vector<uint8_t> patchRam;
		std::vector<uint8_t> initialFlash;
		bool containsFlash = false;
		if(_context->m_model == MachineModel::Monomachine)
		{
			DecodedState decoded;
			if(!decodeState(decoded, _state, {}, _context->m_model, _type))
				return fail("The Monomachine project payload is invalid or incompatible.");
			patchRam = std::move(decoded.patchRam);
			initialFlash = std::move(decoded.userFlash);
			containsFlash = decoded.containsFlash;
		}
		else
		{
			auto stateRom = loadStateRom(_context->m_romData, _context->m_romName,
				_context->m_model);
			if(!stateRom.isValid())
				return fail("The Machinedrum firmware needed to restore this project is unavailable or invalid.");
			// Resolve the base image the project was saved against: the stock ROM, or
			// the registered variant named by the overlay fingerprint (variant filenames
			// ARE the fingerprint, so this is one file read, not a directory scan).
			// Anything else keeps legacy stock behaviour; an unknown fingerprint fails
			// loudly like any foreign firmware.
			const auto& homePath = _context->m_homePath;
			const auto model = _context->m_model;
			uint64_t overlayFp = 0;
			const bool hasOverlayFp = stateOverlayRomFingerprint(overlayFp,
				_state);
			const std::vector<uint8_t>* baseImage = &stateRom.data();
			OsVariant stateVariant;
			uint64_t stateVariantFp = 0;
			if(hasOverlayFp && overlayFp != fingerprint(stateRom.data()))
			{
				if(!resolveVariantBase(stateVariant, homePath, model,
					overlayFp))
					return fail("The Machinedrum project payload is corrupt, incompatible, or belongs to different firmware.");
				baseImage = &stateVariant.image;
				stateVariantFp = overlayFp;
			}
			DecodedState decoded;
			if(!decodeState(decoded, _state, *baseImage, _context->m_model, _type))
				return fail("The Machinedrum project payload is corrupt, incompatible, or belongs to different firmware.");
			patchRam = std::move(decoded.patchRam);
			// Reinstating the user RAM preserved across an OS switch: the
			// settled reboot after a target's first-run initialization must
			// run the user's kits, not the factory kit store that
			// initialization just wrote over them.
			if(_userRamOverride.size() == g_patchRamStateSize)
			{
				patchRam = _userRamOverride;
				std::fprintf(stderr,
					"[MD] reinstating the preserved user RAM (kits/patterns/songs)\n");
			}
			containsFlash = decoded.containsFlash;
			auto factory = loadInitialMdFlash(stateRom, stateVariantFp != 0
				? variantFactoryCacheFilename(homePath, model, stateVariantFp)
				: mdFlashCacheFilename(homePath, model),
				stateVariantFp != 0 ? baseImage : nullptr);
			// A captured snapshot carries the fingerprint of the image it
			// was encoded against (zero for stock). A snapshot from another
			// OS than this project is ignored; the files below decide.
			FactoryFlashSnapshot noSnapshot;
			const auto* snapshot = &_factoryFlash;
			if((!snapshot->cache.empty() || !snapshot->baseline.empty())
				&& snapshot->baseFingerprint != stateVariantFp)
				snapshot = &noSnapshot;
			// The snapshot (when kept above) is already bound to this
			// project's base image: stock resolves to the ROM, a variant
			// fingerprint to its registered image.
			const std::vector<uint8_t>* factoryBase = baseImage;
			OsVariant factoryVariant;
			if(snapshot->baseFingerprint != 0)
			{
				if(!resolveVariantBase(factoryVariant, homePath, model,
					snapshot->baseFingerprint))
					return fail("The captured OS variant image is missing or invalid.");
				factoryBase = &factoryVariant.image;
			}
			if(factory.cache.empty() && !snapshot->cache.empty())
			{
				if(!decodeFactoryFlashCache(factory.flash, snapshot->cache,
					*factoryBase))
					return fail("The captured Machinedrum factory-flash cache is invalid for this firmware.");
				factory.cache = snapshot->cache;
			}
			else if(factory.cache.empty() && !snapshot->baseline.empty())
			{
				factory.flash = snapshot->baseline;
				if(!encodeFactoryFlashCache(factory.cache, factory.flash,
					*factoryBase))
					return fail("The Machinedrum factory-flash baseline could not be prepared.");
			}
			FlashSectorOverlay pending;
			if(containsFlash)
			{
				if(!factory.flash.empty())
				{
					if(!applyFlashOverlay(initialFlash, decoded.flashOverlay,
						factory.flash, *baseImage))
						return fail("The project sample-flash overlay does not match the Machinedrum factory baseline.");
				}
				else if(decoded.flashOverlay.sectors.size()
					== g_romSize / g_uwFlashSectorSize)
				{
					// A complete overlay is baseline-independent. Materialize it before
					// the replacement starts so firmware boots from one coherent project.
					if(!applyFlashOverlay(initialFlash, decoded.flashOverlay,
						*baseImage, *baseImage))
						return fail("The complete project sample-flash image could not be materialized.");
				}
				else
					pending = std::move(decoded.flashOverlay);
			}
			else if(!factory.flash.empty())
				initialFlash = factory.flash;
			else if(stateVariantFp != 0)
			{
				// First visit: no cache exists for this variant yet. Boot
				// the raw registered image; firmware initializes once.
				initialFlash = stateVariant.image;
			}
			else
			{
				// No factory image and no project overlay: boot a previously
				// MIDI-upgraded OS image for this ROM when one was persisted.
				initialFlash = loadInitialOsImage(_context->m_romData,
					_context->m_romName, _context->m_model, _context->m_homePath);
			}

		// The replacement boots whichever image was just resolved, so it is a
		// variant boot exactly when that image is a registered variant. This must be
		// settled before construction: Hardware::expectsFactoryFlashInitialization()
		// keys off it, and a variant machine still doing first-run initialization
		// has to keep expecting it. Omitting the flag defaulted it to false, so
		// restoring project state silently replaced an un-settled OS with a machine
		// that believed it was settled: the settle could never run, no factory
		// baseline was captured, and SysEx import stayed refused for that OS.
		uint64_t preparedFp = stateVariantFp;
		if(!initialFlash.empty())
			findOsVariantByImage(preparedFp, homePath, model, initialFlash);

		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName, _context->m_model, patchRam,
			std::shared_ptr<FrontPanelPublisher>{},
			initialFlash, factory.cache, pending, std::vector<uint8_t>{},
			preparedFp != 0);
		if(!replacement->isValid())
			return fail("The replacement Machinedrum machine rejected the restored firmware or memory image.");
		if(_bootHold)
			replacement->seedBootHoldPanel(_bootHold->row, _bootHold->mask);
		auto prepared = std::unique_ptr<PreparedState>(
			new PreparedState(_context, std::move(replacement),
				containsFlash));
		std::string error;
		if(!attachPreparedPlusDrive(*prepared, &error))
			return fail(error.c_str());
		// The committed machine will run this OS: a materialized image
		// identifies itself by content, otherwise the resolved project
		// base decides (patch-only states carry no image yet).
		prepared->m_osVariantFingerprint = preparedFp;
		return prepared;
	}

		auto replacement = std::make_unique<Hardware>(
			_context->m_romData, _context->m_romName, _context->m_model, patchRam,
			std::shared_ptr<FrontPanelPublisher>{},
			loadInitialOsImage(_context->m_romData, _context->m_romName,
				_context->m_model, _context->m_homePath),
			std::vector<uint8_t>{}, FlashSectorOverlay{}, initialFlash);
		if(!replacement->isValid())
			return fail("The replacement Monomachine rejected the restored firmware or memory image.");
	if(_bootHold)
		replacement->seedBootHoldPanel(_bootHold->row, _bootHold->mask);
	auto preparedMm = std::unique_ptr<PreparedState>(
		new PreparedState(std::move(_context), std::move(replacement),
			containsFlash));
	std::string error;
	if(!attachPreparedPlusDrive(*preparedMm, &error))
		return fail(error.c_str());
	return preparedMm;
	}

	bool Device::commitPreparedState(PreparedState& _prepared)
	{
		if(_prepared.m_committed || _prepared.m_context != m_preparationContext
			|| !_prepared.m_hardware
			|| !_prepared.m_hardware->isValid())
			return false;

		const char* const plusDriveOpt = std::getenv("GEARMULATOR_MDMM_PLUSDRIVE");
		const bool plusDriveEnabled = (plusDriveOpt == nullptr || plusDriveOpt[0] != '0')
			&& m_preparationContext && !m_preparationContext->m_homePath.empty();
		if(!plusDriveEnabled)
		{
			// A mid-preparation opt-out is a real rejection, not a reason to
			// carry the old live responder into the replacement.
			if(m_hardware)
				m_hardware->getUC().getSim().setPlusDrive(nullptr);
			_prepared.m_hardware->getUC().getSim().setPlusDrive(nullptr);
			_prepared.m_plusDrive.reset();
			m_plusDrive.reset();
		}

		const bool hadLivePlusDrive = static_cast<bool>(m_plusDrive);
		const auto discardUncommittedResponder = [&]()
		{
			if(hadLivePlusDrive)
				return;
			if(_prepared.m_hardware)
				_prepared.m_hardware->getUC().getSim().setPlusDrive(nullptr);
			m_plusDrive.reset();
		};
		const auto clockPercent = getDspClockPercent();
		_prepared.m_hardware->requestRamRecordingMode(m_ramRecordingMode);
		_prepared.m_hardware->getDspMixer().getPeriph().getEssiClock()
			.setSpeedPercent(clockPercent);
		// A prepared responder is a speculative overlay. If this Device did not
		// already own a committed store (possible for a detached/replacement
		// transaction), materialize the process-wide primary before rebasing it.
		if(!m_plusDrive && _prepared.m_plusDrive && m_preparationContext
			&& !m_preparationContext->m_homePath.empty())
		{
			m_plusDrive.emplace();
			m_plusDrive->setMode(PlusDrive::Mode::Canned);
			m_plusDrive->setServeFlash(true);
			m_plusDrive->setModel(m_model);
			std::string error;
			if(!m_plusDrive->setBackingPath(plusdrive::filename(
				m_preparationContext->m_homePath, m_model), false, &error))
			{
				m_plusDrive.reset();
				std::fprintf(stderr, "[MD] +Drive commit could not open committed store: %s\n",
					error.c_str());
				return false;
			}
		}
		if(m_plusDrive && _prepared.m_plusDrive)
		{
			std::string error;
			if(!m_plusDrive->canCommitPrepared(*_prepared.m_plusDrive, &error))
			{
				discardUncommittedResponder();
				std::fprintf(stderr, "[MD] +Drive prepared store is stale: %s\n",
					error.c_str());
				return false;
			}
		}

		// If the environment disabled +Drive for the candidate, do not discover
		// a new I/O failure only after swapping Hardware. Prepare the fallback
		// responder while the old machine is still authoritative.
		if(!m_plusDrive && !_prepared.m_plusDrive && m_preparationContext
			&& !m_preparationContext->m_homePath.empty())
		{
			std::string error;
			if(!attachPlusDrive(*_prepared.m_hardware, m_preparationContext->m_homePath,
				m_model, &error))
			{
				discardUncommittedResponder();
				return false;
			}
		}

		bool exchangedPersistentFlash = false;
		if(m_model == MachineModel::Machinedrum && !_prepared.m_containsFlash)
		{
			// Patch-only and legacy states preserve the current sample flash. Both
			// machines are stopped under the outer Device lock, so exchange ownership
			// of the multi-megabyte backing stores and factory-capture progress in O(1).
			if(!_prepared.m_hardware->exchangePersistentFlashState(*m_hardware))
			{
				discardUncommittedResponder();
				return false;
			}
			exchangedPersistentFlash = true;
		}
		if(m_plusDrive && _prepared.m_plusDrive)
		{
			std::string error;
			if(!m_plusDrive->commitPrepared(*_prepared.m_plusDrive, &error))
			{
				if(exchangedPersistentFlash)
					(void)_prepared.m_hardware->exchangePersistentFlashState(*m_hardware);
				discardUncommittedResponder();
				std::fprintf(stderr, "[MD] +Drive prepared store could not be committed: %s\n",
					error.c_str());
				return false;
			}
			if(!m_plusDrive->persistenceError().empty())
				std::fprintf(stderr, "[MD] +Drive prepared data is committed in memory but pending persistence: %s\n",
					m_plusDrive->persistenceError().c_str());
		}

		m_frontPanelPublisher->reset();
		_prepared.m_hardware->setFrontPanelPublisher(m_frontPanelPublisher);
		m_hardware.swap(_prepared.m_hardware);
		++m_hardwareEpoch;
		// An OS switch hands the carried user RAM to the replacement machine
		// after it was constructed, so a host that had no canonical copy yet
		// must adopt it here. Kits are machine state rather than OS content:
		// without this the host has no authoritative copy of the user's RAM and
		// a target that reformats its own kit store erases their kits for good.
		if(m_canonicalUserRam.empty())
		{
			const auto live = m_hardware->copyPatchRam();
			if(!live.empty() && std::any_of(live.begin(), live.end(),
				[](const uint8_t _byte) { return _byte != 0; }))
				m_canonicalUserRam = live;
		}
		// The prepared machine used the same committed DriveStore (through its
		// overlay), so retain the live protocol owner and only repoint the new
		// Sim at it. Reset all per-Sim framing/command state at the boundary.
		if(m_plusDrive)
		{
			m_plusDrive->resetTransaction();
			m_hardware->getUC().getSim().setPlusDrive(&*m_plusDrive);
		}
		else if(m_preparationContext && !m_preparationContext->m_homePath.empty())
			attachPlusDrive(*m_hardware, m_preparationContext->m_homePath, m_model);
		// The candidate's overlay has either been rebased into m_plusDrive or
		// deliberately rejected. Detach the retired Sim before destroying the
		// endpoint so its pointer can never outlive the object.
		if(_prepared.m_hardware)
			_prepared.m_hardware->getUC().getSim().setPlusDrive(nullptr);
		_prepared.m_plusDrive.reset();
		m_liveVariantFingerprint = _prepared.m_osVariantFingerprint;
		m_preservedUserRam = std::move(_prepared.m_preservedUserRam);
		_prepared.m_committed = true;
		return true;
	}

	void Device::setRamRecordingMode(const RamRecordingMode _mode)
	{
		m_ramRecordingMode = m_model == MachineModel::Machinedrum
			? _mode : RamRecordingMode::Original;
		if(m_hardware)
			m_hardware->requestRamRecordingMode(m_ramRecordingMode);
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware)
			m_deferredPreparedState->m_hardware->requestRamRecordingMode(m_ramRecordingMode);
	}

	bool Device::commitDeferredStateRestore(PreparedState& _prepared,
		const uint64_t _generation)
	{
		if(_generation != m_deferredStateGeneration
			|| m_restoreStatus != ProjectStateRestoreStatus::Finalizing)
			return false;
		if(!commitPreparedState(_prepared))
		{
			failProjectStateRestore("The validated project state could not be committed.");
			return false;
		}
		clearProjectStateRestore();
		return true;
	}

	bool Device::rejectDeferredStateRestore(const uint64_t _generation,
		std::string _error)
	{
		if(_generation != m_deferredStateGeneration
			|| m_restoreStatus != ProjectStateRestoreStatus::Finalizing)
			return false;
		failProjectStateRestore(std::move(_error));
		return true;
	}

	void Device::clearProjectStateRestore()
	{
		m_deferredPreparedState.reset();
		m_requestedState.reset();
		m_restoreStatus = ProjectStateRestoreStatus::Idle;
		m_restoreError.clear();
	}

	void Device::failProjectStateRestore(std::string _error)
	{
		m_deferredPreparedState.reset();
		m_requestedState.reset();
		m_restoreStatus = ProjectStateRestoreStatus::Failed;
		m_restoreError = std::move(_error);
	}

	bool Device::matchesUserSysexImport(const SysexImportTicket& ticket) const
	{
		return ticket.request && ticket == m_sysexTicket && ticket.device == m_sysexDeviceId
			&& ticket.hardware == m_hardwareEpoch && ticket.restore == m_deferredStateGeneration;
	}

	std::optional<SysexImportTicket> Device::beginUserSysexImport()
	{
		if(isProjectStateRestorePending() || m_hardware->isMidiSysexTransferActive()) return {};
		m_sysexTicket = {m_sysexDeviceId, m_hardwareEpoch, m_deferredStateGeneration, m_sysexTicket.request + 1};
		m_sysexStarted = m_sysexPendingCancelled = false;
		return m_sysexTicket;
	}

	SysexImportStartResult Device::startUserSysexImport(const SysexImportTicket& ticket,
		PreparedMidiSysexTransfer& transfer, bool receiveModeConfirmed)
	{
		using Result = SysexImportStartResult;
		if(!matchesUserSysexImport(ticket) || m_sysexStarted || m_sysexPendingCancelled) return Result::StaleRequest;
		if(transfer.model() != m_model) return Result::WrongModel;
		if(isProjectStateRestorePending()) return Result::Restoring;
		if(!isValid() || !m_hardware->isFirmwareMidiReady()) return Result::NotReady;
		if(!receiveModeConfirmed && (m_model == MachineModel::Monomachine
			|| transfer.contains(MidiSysexMessageKind::SdsHeader))) return Result::ConfirmationRequired;
		if(!m_hardware->startMidiSysexTransfer(transfer)) return Result::Busy;
		m_sysexStarted = true;
		return Result::Started;
	}

	bool Device::cancelUserSysexImport(const SysexImportTicket& ticket, std::vector<uint8_t>& retired)
	{
		if(!matchesUserSysexImport(ticket) || m_sysexPendingCancelled) return false;
		if(!m_sysexStarted) { m_sysexPendingCancelled = true; return true; }
		return m_hardware->cancelMidiSysexTransfer(retired);
	}

	bool Device::resumeUserSysexImport(const SysexImportTicket& ticket, uint32_t transferId,
		size_t receiveStep, bool receiveModeConfirmed)
	{
		return matchesUserSysexImport(ticket) && m_sysexStarted && receiveModeConfirmed
			&& !isProjectStateRestorePending() && m_hardware->isFirmwareMidiReady()
			&& m_hardware->resumeMidiSysexReceiveMode(transferId, receiveStep);
	}

	bool Device::retireUserSysexImport(const SysexImportTicket& ticket, std::vector<uint8_t>& retired)
	{
		return matchesUserSysexImport(ticket) && m_sysexStarted
			&& m_hardware->retireMidiSysexTransferPayload(retired);
	}

	SysexImportProgress Device::userSysexImportProgress() const
	{
		SysexImportProgress result;
		result.ticket = m_sysexTicket;
		if(!m_sysexTicket.request) return result;
		if(!matchesUserSysexImport(m_sysexTicket))
		{
			result.stage = SysexImportStage::Invalidated;
			return result;
		}
		if(!m_sysexStarted)
		{
			result.stage = m_sysexPendingCancelled ? SysexImportStage::Cancelled : SysexImportStage::Preparing;
			return result;
		}
		static_cast<MidiSysexTransferProgress&>(result) = m_hardware->getMidiSysexTransferProgress();
		switch(result.state)
		{
		case MidiSysexTransferState::Complete: result.stage = SysexImportStage::DeliveredUnverified; break;
		case MidiSysexTransferState::Cancelled: result.stage = SysexImportStage::Cancelled; break;
		case MidiSysexTransferState::Failed: result.stage = SysexImportStage::Failed; break;
		case MidiSysexTransferState::WaitingForReceiveMode: result.stage = SysexImportStage::AwaitingReceiveMode; break;
		default: result.stage = SysexImportStage::Transferring; break;
		}
		return result;
	}

	uint32_t Device::getChannelCountIn()
	{
		return 2;
	}

	uint32_t Device::getChannelCountOut()
	{
		return 6;
	}

	bool Device::setDspClockPercent(const uint32_t _percent)
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().setSpeedPercent(_percent);
	}

	uint32_t Device::getDspClockPercent() const
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().getSpeedPercent();
	}

	uint64_t Device::getDspClockHz() const
	{
		return m_hardware->getDspMixer().getPeriph().getEssiClock().getSpeedInHz();
	}

	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		m_hardware->readMidiOut(_midiOut);
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, const size_t _samples)
	{
		m_hardware->processAudio(_inputs, _outputs,
			static_cast<uint32_t>(_samples), getExtraLatencySamples());
		if(m_deferredPreparedState && m_deferredPreparedState->m_hardware
			&& m_deferredPreparedState->m_hardware->isProjectStateRestorePending())
		{
			synthLib::RealtimeInstrumentation::DeferredCandidateScope instrumentation(
				static_cast<uint32_t>(_samples));
			m_deferredPreparedState->m_hardware->advance(
				static_cast<uint32_t>(_samples));
		}
	}

	void Device::extraLatencyChanged()
	{
		m_hardware->retimeMidi(getExtraLatencySamples());
	}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response)
	{
		if(_ev.sysex.empty())
		{
			const auto status = static_cast<uint8_t>(_ev.a & 0xf0);

			// Native Program Change selects a firmware pattern independently of the
			// host's preset list. Forward it by default so firmware can apply its own
			// receive-enable/channel settings. Embedders may explicitly opt out.
			if(m_model != MachineModel::Monomachine
				&& status == synthLib::M_PROGRAMCHANGE
				&& !m_nativeProgramChangesEnabled)
				return true;
		}

		return m_hardware->scheduleMidi(_ev, getExtraLatencySamples());
	}
}
