#include "mdosvariant.h"

#include "mdstate.h"

#include "baseLib/filesystem.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace md
{
	namespace
	{
		uint64_t fingerprintBytes(const std::vector<uint8_t>& _data)
		{
			uint64_t fingerprint = 14695981039346656037ull;
			for(const auto byte : _data)
			{
				fingerprint ^= byte;
				fingerprint *= 1099511628211ull;
			}
			return fingerprint;
		}

		const char* modelPrefix(const MachineModel _model)
		{
			return _model == MachineModel::Monomachine ? "mm" : "md";
		}
	}

	bool assembleOsVariant(OsVariant& _variant,
		const std::vector<uint8_t>& _stockRom,
		const OsUpgradeDecodeResult& _decoded, const std::string& _sourceName)
	{
		OsVariant variant;
		if(_stockRom.size() != g_romSize || _decoded.messages.empty())
			return false;
		variant.model = _decoded.model;
		variant.image = _stockRom;
		if(_decoded.baseAddress != g_variantBaseAddress)
			return false;
		for(const auto& message : _decoded.messages)
		{
			if(message.address < g_variantBaseAddress
				|| message.address + message.data.size() > g_variantSplitAddress)
				return false;
			std::copy(message.data.begin(), message.data.end(),
				variant.image.begin() + message.address);
		}
		variant.fingerprint = fingerprintBytes(variant.image);
		variant.sourceName = _sourceName;
		_variant = std::move(variant);
		return true;
	}

	bool splitSidecarUpgrade(OsVariant& _variant, std::vector<uint8_t>& _overlay,
		const std::vector<uint8_t>& _sidecar, const std::vector<uint8_t>& _stockRom,
		const MachineModel _model, const std::string& _sourceName)
	{
		if(_model != MachineModel::Machinedrum || _sidecar.size() != g_romSize
			|| _stockRom.size() != g_romSize)
			return false;
		// The sidecar must be based on this exact stock ROM: bootloader and
		// shared payload are never rewritten by upgrades or users.
		if(!std::equal(_sidecar.begin(), _sidecar.begin() + 0x4000,
				_stockRom.begin())
			|| !std::equal(_sidecar.begin() + 0x100000,
				_sidecar.begin() + g_variantSplitAddress, _stockRom.begin() + 0x100000))
			return false;
		OsVariant variant;
		variant.model = _model;
		variant.image = _sidecar;
		// Restore everything at/above the split from stock: no user bytes may
		// ever persist into a variant file.
		std::copy(_stockRom.begin() + g_variantSplitAddress, _stockRom.end(),
			variant.image.begin() + g_variantSplitAddress);
		variant.fingerprint = fingerprintBytes(variant.image);
		variant.sourceName = _sourceName;
		_overlay.assign(_sidecar.begin() + g_variantSplitAddress, _sidecar.end());
		_variant = std::move(variant);
		return true;
	}

	std::string variantFilename(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint)
	{
		if(_homePath.empty())
			return {};
		char suffix[20];
		std::snprintf(suffix, sizeof(suffix), "%016llx",
			static_cast<unsigned long long>(_fingerprint));
		return baseLib::filesystem::validatePath(_homePath)
			+ "os-variants/" + modelPrefix(_model) + '-' + suffix + ".bin";
	}

	// The human-readable source name lives in a sidecar: the variant file
	// itself stays a pure flash image. Best-effort; a missing or unreadable
	// sidecar just means the UI falls back to the fingerprint.
	void writeVariantLabel(const std::string& _variantFilename,
		const std::string& _sourceName)
	{
		if(_sourceName.empty())
			return;
		const auto labelFile = _variantFilename + ".label";
		std::vector<uint8_t> existing;
		if(!baseLib::filesystem::readFile(existing, labelFile))
		{
			baseLib::filesystem::writeFileAtomic(labelFile,
				std::vector<uint8_t>(_sourceName.begin(), _sourceName.end()));
		}
	}

	bool saveOsVariant(std::string& _filename, const std::string& _homePath,
		const OsVariant& _variant, std::string& _error)
	{
		_error.clear();
		_filename.clear();
		if(_variant.image.size() != g_romSize)
		{
			_error = "variant image has wrong size";
			return false;
		}
		const auto filename = variantFilename(_homePath, _variant.model,
			_variant.fingerprint);
		if(filename.empty())
		{
			_error = "variant has no home path";
			return false;
		}
		std::vector<uint8_t> existing;
		if(baseLib::filesystem::readFile(existing, filename))
		{
			if(existing == _variant.image)
			{
				// Idempotent re-registration also heals a missing label
				// sidecar (e.g. images stored before sidecars existed).
				writeVariantLabel(filename, _variant.sourceName);
				_filename = filename;
				return true;
			}
			_error = "variant file collision, refusing to overwrite";
			return false;
		}
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(filename));
		if(!baseLib::filesystem::writeFileExclusive(filename, _variant.image))
		{
			_error = "could not store variant image";
			return false;
		}
		writeVariantLabel(filename, _variant.sourceName);
		_filename = filename;
		return true;
	}

	bool loadOsVariant(OsVariant& _variant, const std::string& _filename,
		const MachineModel _model)
	{
		OsVariant variant;
		variant.model = _model;
		if(_filename.empty()
			|| !baseLib::filesystem::readFile(variant.image, _filename)
			|| variant.image.size() != g_romSize)
			return false;
		variant.fingerprint = fingerprintBytes(variant.image);
		_variant = std::move(variant);
		return true;
	}

	bool listOsVariants(std::vector<OsVariantInfo>& _variants,
		const std::string& _homePath)	{
		_variants.clear();
		if(_homePath.empty())
			return false;
		std::vector<std::string> files;
		if(!baseLib::filesystem::findFiles(files,
			baseLib::filesystem::validatePath(_homePath) + "os-variants/",
			".bin", g_romSize, g_romSize))
			return true;	// no store yet is not an error
		for(const auto& path : files)
		{
			const auto name = path.substr(path.find_last_of("/\\") + 1);
			OsVariantInfo info;
			if(name.compare(0, 3, "md-") == 0)
				info.model = MachineModel::Machinedrum;
			else if(name.compare(0, 3, "mm-") == 0)
				info.model = MachineModel::Monomachine;
			else
				continue;
			unsigned long long fingerprint = 0;
			if(std::sscanf(name.c_str() + 3, "%16llx.bin", &fingerprint) != 1)
				continue;
			info.fingerprint = static_cast<uint64_t>(fingerprint);
			info.filename = path;
			std::vector<uint8_t> labelBytes;
			if(baseLib::filesystem::readFile(labelBytes, path + ".label")
				&& !labelBytes.empty() && labelBytes.size() <= 128)
			{
				const std::string label(labelBytes.begin(), labelBytes.end());
				const auto printable = std::all_of(label.begin(), label.end(),
					[](const unsigned char _c) { return _c >= 0x20 && _c < 0x7f; });
				if(printable)
					info.label = label;
			}
			_variants.push_back(std::move(info));
		}
		return true;
	}

	std::string variantFactoryCacheFilename(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint)
	{
		if(_homePath.empty() || _fingerprint == 0)
			return {};
		char suffix[20];
		std::snprintf(suffix, sizeof(suffix), "%016llx",
			static_cast<unsigned long long>(_fingerprint));
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/" + modelPrefix(_model) + '-' + suffix + "-factory-v1.cache";
	}

	namespace
	{
		bool osRegionMatches(const std::vector<uint8_t>& _candidate,
			const std::vector<uint8_t>& _expected)
		{
			return _candidate.size() == g_romSize
				&& _expected.size() == g_romSize
				&& g_variantSplitAddress <= _candidate.size()
				&& std::equal(_candidate.begin() + g_variantBaseAddress,
					_candidate.begin() + g_variantSplitAddress,
					_expected.begin() + g_variantBaseAddress);
		}
	}

	std::string variantNeedsNoInitFilename(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint)
	{
		if(_homePath.empty() || _fingerprint == 0)
			return {};
		char suffix[20];
		std::snprintf(suffix, sizeof(suffix), "%016llx",
			static_cast<unsigned long long>(_fingerprint));
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/" + modelPrefix(_model) + '-' + suffix + "-noinit.marker";
	}

	bool variantNeedsNoInit(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint)
	{
		const auto filename = variantNeedsNoInitFilename(_homePath, _model,
			_fingerprint);
		return !filename.empty() && baseLib::filesystem::exists(filename);
	}

	bool markVariantNeedsNoInit(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint)
	{
		const auto filename = variantNeedsNoInitFilename(_homePath, _model,
			_fingerprint);
		return !filename.empty()
			&& baseLib::filesystem::writeFile(filename,
				std::vector<uint8_t>{ 0x4e, 0x6f, 0x49, 0x6e, 0x69, 0x74 });
	}

	std::string osHibernationFilename(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint)	{
		if(_homePath.empty())
			return {};
		char suffix[20];
		std::snprintf(suffix, sizeof(suffix), "%016llx",
			static_cast<unsigned long long>(_fingerprint));
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/" + modelPrefix(_model) + "-os-" + suffix + ".hibernation";
	}

	bool loadOsHibernation(std::vector<uint8_t>& _flash,
		const std::string& _homePath, const MachineModel _model,
		const uint64_t _fingerprint, const std::vector<uint8_t>& _expectedImage)
	{
		_flash.clear();
		const auto filename = osHibernationFilename(_homePath, _model,
			_fingerprint);
		std::vector<uint8_t> flash;
		if(filename.empty()
			|| !baseLib::filesystem::readFile(flash, filename)
			|| !osRegionMatches(flash, _expectedImage))
			return false;
		_flash = std::move(flash);
		return true;
	}

	bool writeOsHibernation(const std::string& _homePath,
		const MachineModel _model, const uint64_t _fingerprint,
		const std::vector<uint8_t>& _flash,
		const std::vector<uint8_t>& _expectedImage)
	{
		const auto filename = osHibernationFilename(_homePath, _model,
			_fingerprint);
		if(filename.empty() || !osRegionMatches(_flash, _expectedImage))
			return false;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(filename));
		return baseLib::filesystem::writeFileAtomic(filename, _flash);
	}

	std::string userPatchRamFilename(const std::string& _homePath,
		const MachineModel _model)
	{
		if(_homePath.empty())
			return {};
		return baseLib::filesystem::validatePath(_homePath)
			+ "nvram/" + modelPrefix(_model) + "-user.patchram";
	}

	bool loadUserPatchRam(std::vector<uint8_t>& _patchRam,
		const std::string& _homePath, const MachineModel _model)
	{
		_patchRam.clear();
		const auto filename = userPatchRamFilename(_homePath, _model);
		std::vector<uint8_t> patchRam;
		if(filename.empty()
			|| !baseLib::filesystem::readFile(patchRam, filename)
			|| patchRam.size() != g_patchRamStateSize)
			return false;
		_patchRam = std::move(patchRam);
		return true;
	}

	bool writeUserPatchRam(const std::string& _homePath,
		const MachineModel _model, const std::vector<uint8_t>& _patchRam)
	{
		const auto filename = userPatchRamFilename(_homePath, _model);
		if(filename.empty() || _patchRam.size() != g_patchRamStateSize)
			return false;
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(filename));
		return baseLib::filesystem::writeFileAtomic(filename, _patchRam);
	}

	bool findOsVariantByImage(uint64_t& _fingerprint,
		const std::string& _homePath, const MachineModel _model,
		const std::vector<uint8_t>& _image)	{
		_fingerprint = 0;
		if(_homePath.empty() || _image.size() != g_romSize
			|| g_variantSplitAddress > _image.size())
			return false;
		std::vector<OsVariantInfo> variants;
		if(!listOsVariants(variants, _homePath))
			return false;
		for(const auto& info : variants)
		{
			if(info.model != _model || info.fingerprint == 0)
				continue;
			OsVariant variant;
			if(!loadOsVariant(variant, info.filename, _model)
				|| variant.fingerprint != info.fingerprint)
				continue;
			if(std::equal(_image.begin() + g_variantBaseAddress,
					_image.begin() + g_variantSplitAddress,
					variant.image.begin() + g_variantBaseAddress))
			{
				_fingerprint = info.fingerprint;
				return true;
			}
		}
		return false;
	}

	bool findOsVariantByCache(uint64_t& _fingerprint,
		const std::string& _homePath, const MachineModel _model,
		const std::vector<uint8_t>& _cache)
	{
		_fingerprint = 0;
		if(_homePath.empty() || _cache.empty())
			return false;
		std::vector<OsVariantInfo> variants;
		if(!listOsVariants(variants, _homePath))
			return false;
		for(const auto& info : variants)
		{
			if(info.model != _model || info.fingerprint == 0)
				continue;
			OsVariant variant;
			if(!loadOsVariant(variant, info.filename, _model)
				|| variant.fingerprint != info.fingerprint)
				continue;
			std::vector<uint8_t> decoded;
			if(decodeFactoryFlashCache(decoded, _cache, variant.image))
			{
				_fingerprint = info.fingerprint;
				return true;
			}
		}
		return false;
	}

	std::string variantSelectionFilename(const std::string& _homePath)
	{
		if(_homePath.empty())
			return {};
		return baseLib::filesystem::validatePath(_homePath) + "os-variant.txt";
	}

	bool readOsVariantSelection(uint64_t& _fingerprint, bool& _found,
		const std::string& _homePath)
	{
		_fingerprint = 0;
		_found = false;
		const auto filename = variantSelectionFilename(_homePath);
		if(filename.empty())
			return false;
		std::vector<uint8_t> data;
		if(!baseLib::filesystem::readFile(data, filename))
			return true;	// missing selection means stock, not an error
		unsigned long long fingerprint = 0;
		if(std::sscanf(reinterpret_cast<const char*>(data.data()),
				"%16llx", &fingerprint) != 1)
			return false;
		_fingerprint = static_cast<uint64_t>(fingerprint);
		_found = _fingerprint != 0;
		return true;
	}

	bool writeOsVariantSelection(const std::string& _homePath,
		const uint64_t _fingerprint)
	{
		const auto filename = variantSelectionFilename(_homePath);
		if(filename.empty())
			return false;
		char text[20];
		std::snprintf(text, sizeof(text), "%016llx\n",
			static_cast<unsigned long long>(_fingerprint));
		baseLib::filesystem::createDirectory(
			baseLib::filesystem::getPath(filename));
		std::vector<uint8_t> bytes(text, text + std::strlen(text));
		std::vector<uint8_t> existing;
		if(baseLib::filesystem::readFile(existing, filename)
			&& existing == bytes)
			return true;
		return baseLib::filesystem::writeFileAtomic(filename, bytes);
	}

	bool clearOsVariantSelection(const std::string& _homePath)
	{
		// Stock is fingerprint zero, which reads back as "not found".
		return writeOsVariantSelection(_homePath, 0);
	}
}
