#include "mdStorageImage.h"

#include "mdLib/mdstate.h"
#include "mdLib/mdplusdrive.h"

#include "baseLib/filesystem.h"

namespace mdJucePlugin::storageImage
{
	namespace
	{
		juce::String describe(const juce::File& _file)
		{
			return _file.getFullPathName().isNotEmpty()
				? _file.getFullPathName() : juce::String("the selected file");
		}
	}

	bool readExact(const juce::File& _source, std::vector<uint8_t>& _bytes,
		juce::String& _error)
	{
		_bytes.clear();
		_error.clear();

		if(!_source.existsAsFile())
		{
			_error = "Storage image was not found: " + describe(_source);
			return false;
		}

		if(_source.getSize() != static_cast<juce::int64>(md::g_patchRamStateSize))
		{
			_error = "Storage image must be exactly 1 MiB";
			return false;
		}

		juce::MemoryBlock raw;
		if(!_source.loadFileAsData(raw)
			|| raw.getSize() != md::g_patchRamStateSize)
		{
			_error = "Could not read the complete storage image: " + describe(_source);
			return false;
		}

		const auto* const begin = static_cast<const uint8_t*>(raw.getData());
		_bytes.assign(begin, begin + raw.getSize());
		return true;
	}

	bool writeExactAtomically(const juce::File& _target,
		const std::vector<uint8_t>& _bytes, juce::String& _error)
	{
		_error.clear();
		if(_bytes.size() != md::g_patchRamStateSize)
		{
			_error = "Recovery storage is not exactly 1 MiB";
			return false;
		}

		const auto directoryResult = _target.getParentDirectory().createDirectory();
		if(directoryResult.failed())
		{
			_error = "Could not create the recovery folder: "
				+ directoryResult.getErrorMessage();
			return false;
		}

		juce::TemporaryFile temporary(_target, juce::TemporaryFile::useHiddenFile);
		auto output = temporary.getFile().createOutputStream();
		if(!output || !output->openedOk())
		{
			_error = "Could not create a temporary recovery file";
			return false;
		}

		const bool wroteAll = output->write(_bytes.data(), _bytes.size());
		output->flush();
		const auto writeStatus = output->getStatus();
		output.reset();
		if(!wroteAll || writeStatus.failed()
			|| temporary.getFile().getSize()
				!= static_cast<juce::int64>(md::g_patchRamStateSize))
		{
			_error = "Could not write the complete recovery image";
			if(writeStatus.failed() && writeStatus.getErrorMessage().isNotEmpty())
				_error += ": " + writeStatus.getErrorMessage();
			return false;
		}

		if(!temporary.overwriteTargetFileWithTemporary())
		{
			_error = "Could not atomically replace the recovery image: " + describe(_target);
			return false;
		}

		if(!_target.existsAsFile()
			|| _target.getSize() != static_cast<juce::int64>(md::g_patchRamStateSize))
		{
			_error = "Recovery image verification failed: " + describe(_target);
			return false;
		}
		return true;
	}

	bool installRecoveryThenCommit(const juce::File& _target,
		const std::vector<uint8_t>& _currentBytes,
		const std::function<bool()>& _commit, juce::String& _error)
	{
		_error.clear();
		if(!_commit)
		{
			_error = "Recovery transaction has no commit operation";
			return false;
		}

		const bool recoveryExisted = _target.existsAsFile();
		std::vector<uint8_t> previousRecovery;
		if(recoveryExisted)
		{
			juce::String readError;
			if(!readExact(_target, previousRecovery, readError))
			{
				_error = "Existing recovery image could not be preserved. " + readError;
				return false;
			}
		}

		juce::String writeError;
		if(!writeExactAtomically(_target, _currentBytes, writeError))
		{
			_error = "Recovery image could not be saved. " + writeError;
			return false;
		}

		if(_commit())
			return true;

		if(recoveryExisted)
		{
			juce::String rollbackError;
			if(writeExactAtomically(_target, previousRecovery, rollbackError))
			{
				_error = "Commit failed; the previous recovery image was restored";
				return false;
			}
			_error = "Commit failed and the previous recovery image could not be restored. "
				+ rollbackError;
			return false;
		}

		if(!_target.exists() || _target.deleteFile())
		{
			_error = "Commit failed; the newly created recovery image was removed";
			return false;
		}
		_error = "Commit failed and the newly created recovery image could not be removed: "
			+ describe(_target);
		return false;
	}

	bool readPlusDrive(const juce::File& _source, std::vector<uint8_t>& _bytes,
		juce::String& _error)
	{
		_bytes.clear();
		_error.clear();
		if(!_source.existsAsFile())
		{
			_error = "+Drive image was not found: " + describe(_source);
			return false;
		}
		const auto size = _source.getSize();
		if(size < 16
			|| size > static_cast<juce::int64>(md::g_plusDriveMaxSerializedBytes))
		{
			_error = "+Drive image must be between 16 bytes and 512 MiB";
			return false;
		}
		std::vector<uint8_t> decoded;
		if(!baseLib::filesystem::readFile(
			decoded, _source.getFullPathName().toStdString())
			|| decoded.size() != static_cast<size_t>(size))
		{
			_error = "Could not read the complete +Drive image: " + describe(_source);
			return false;
		}
		if(!md::PlusDrive::validateStorage(decoded))
		{
			_error = "The selected file is not a valid sparse MDPD +Drive image";
			return false;
		}
		_bytes.swap(decoded);
		return true;
	}

	bool writePlusDriveAtomically(const juce::File& _target,
		const std::vector<uint8_t>& _bytes, juce::String& _error)
	{
		_error.clear();
		if(_target == juce::File() || _target.isDirectory())
		{
			_error = "Choose a +Drive image filename, not a folder";
			return false;
		}
		if(_bytes.size() < 16 || _bytes.size() > md::g_plusDriveMaxSerializedBytes
			|| !md::PlusDrive::validateStorage(_bytes))
		{
			_error = "The live +Drive data is not a valid bounded sparse image";
			return false;
		}
		const auto directoryResult = _target.getParentDirectory().createDirectory();
		if(directoryResult.failed())
		{
			_error = "Could not create the +Drive image folder: "
				+ directoryResult.getErrorMessage();
			return false;
		}
		if(!baseLib::filesystem::writeFileAtomic(
			_target.getFullPathName().toStdString(), _bytes))
		{
			_error = "Could not flush and atomically replace the +Drive image: "
				+ describe(_target);
			return false;
		}
		if(!_target.existsAsFile()
			|| _target.getSize() != static_cast<juce::int64>(_bytes.size()))
		{
			_error = "The atomically replaced +Drive image failed verification: "
				+ describe(_target);
			return false;
		}
		return true;
	}
}
