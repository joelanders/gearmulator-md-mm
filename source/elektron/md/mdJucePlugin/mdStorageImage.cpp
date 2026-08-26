#include "mdStorageImage.h"

#include "mdLib/mdstate.h"

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
}
