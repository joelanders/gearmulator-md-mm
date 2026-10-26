#include "mdStandalonePlusDrivePersistence.h"

#include "mdLib/mdplusdrive.h"

#include "baseLib/filesystem.h"

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	std::vector<uint8_t> makeImage(const uint32_t _sector, const uint8_t _value)
	{
		md::PlusDrive drive;
		auto image = drive.copyStorage();
		image[15] = 1;
		image.push_back(static_cast<uint8_t>(_sector >> 24));
		image.push_back(static_cast<uint8_t>(_sector >> 16));
		image.push_back(static_cast<uint8_t>(_sector >> 8));
		image.push_back(static_cast<uint8_t>(_sector));
		image.insert(image.end(), 512, _value);
		return image;
	}

	bool waitForImage(const juce::File& _file, const std::vector<uint8_t>& _expected)
	{
		for(int attempt = 0; attempt < 120; ++attempt)
		{
			std::vector<uint8_t> actual;
			if(baseLib::filesystem::readFile(
				actual, _file.getFullPathName().toStdString()) && actual == _expected)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}

	bool waitForFile(const juce::File& _file)
	{
		for(int attempt = 0; attempt < 120; ++attempt)
		{
			if(_file.existsAsFile())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		return false;
	}

	int fail(const char* const _message)
	{
		std::fprintf(stderr, "mdStandalonePlusDrivePersistenceTest: %s\n", _message);
		return 1;
	}

	struct LiveState final
	{
		std::mutex operationMutex;
		std::mutex stateMutex;
		mdJucePlugin::StandalonePlusDrivePersistence::Snapshot live;

		mdJucePlugin::StandalonePlusDrivePersistence makePersistence(
			const juce::File& _file)
		{
			return {
				_file, operationMutex,
				[this](const bool _includeData,
					mdJucePlugin::StandalonePlusDrivePersistence::Snapshot& _snapshot)
				{
					std::lock_guard lock(stateMutex);
					_snapshot.hardwareEpoch = live.hardwareEpoch;
					_snapshot.generation = live.generation;
					_snapshot.dirty = live.dirty;
					if(_includeData)
						_snapshot.data = live.data;
					return true;
				},
				[this](const uint64_t _epoch, const uint64_t _generation)
				{
					std::lock_guard lock(stateMutex);
					if(live.hardwareEpoch == _epoch && live.generation == _generation)
						live.dirty = false;
				}
			};
		}
	};

	int runOwnerChild(const juce::File& _checkpoint, const juce::File& _ready,
		const juce::File& _release)
	{
		LiveState state;
		state.live = {1, 1, false, makeImage(7, 0x5a)};
		auto persistence = state.makePersistence(_checkpoint);
		const auto started = persistence.start();
		if(!started.writable)
			return fail("owner child could not acquire checkpoint");
		persistence.requestFlush(true);
		if(!waitForImage(_checkpoint, state.live.data)
			|| !_ready.replaceWithText("ready"))
			return fail("owner child did not become ready");
		for(int attempt = 0; attempt < 400 && !_release.existsAsFile(); ++attempt)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if(!_release.existsAsFile())
			return fail("owner child timed out waiting for release");
		persistence.stop();
		return 0;
	}

	int runProbeChild(const juce::File& _checkpoint, const bool _expectWriter)
	{
		LiveState state;
		state.live = {1, 1, false, makeImage(7, 0x5a)};
		auto persistence = state.makePersistence(_checkpoint);
		const auto started = persistence.start();
		const bool isWriter = started.writable && persistence.ownsWriter();
		if(isWriter != _expectWriter)
			return fail(_expectWriter
				? "fresh process could not take over released checkpoint"
				: "separate process acquired an already-owned checkpoint");
		if(!_expectWriter && !persistence.status().containsIgnoreCase("restart"))
			return fail("read-only ownership status did not explain recovery");
		persistence.stop();
		return 0;
	}

	int runChangeChild(const juce::File& _checkpoint, const juce::File& _ready)
	{
		LiveState state;
		state.live = {1, 1, false, makeImage(7, 0x5a)};
		auto persistence = state.makePersistence(_checkpoint);
		const auto started = persistence.start();
		if(!started.writable || !started.hasInitialImage
			|| started.initialImage != state.live.data)
			return fail("change child did not restore its initial checkpoint");
		{
			std::lock_guard lock(state.stateMutex);
			state.live.generation = 2;
			state.live.dirty = true;
			state.live.data = makeImage(9, 0x27);
		}
		if(!_ready.replaceWithText("changed"))
			return fail("change child could not signal readiness");
		for(int attempt = 0; attempt < 400; ++attempt)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		return fail("change child was not terminated by its parent");
	}

	bool startChild(juce::ChildProcess& _child, const juce::StringArray& _arguments)
	{
		return _child.start(_arguments,
			juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr);
	}

	juce::StringArray childArguments(
		const std::initializer_list<juce::String> _arguments)
	{
		juce::StringArray result;
		for(const auto& argument : _arguments)
			result.add(argument);
		return result;
	}

	bool childSucceeded(juce::ChildProcess& _child)
	{
		return _child.waitForProcessToFinish(10000) && _child.getExitCode() == 0;
	}
}

int main(const int _argc, const char* const* _argv)
{
	if(_argc >= 3)
	{
		const std::string mode(_argv[1]);
		if(mode == "--owner" && _argc == 5)
			return runOwnerChild(juce::File(_argv[2]), juce::File(_argv[3]),
				juce::File(_argv[4]));
		if(mode == "--probe-reader" && _argc == 3)
			return runProbeChild(juce::File(_argv[2]), false);
		if(mode == "--probe-writer" && _argc == 3)
			return runProbeChild(juce::File(_argv[2]), true);
		if(mode == "--change" && _argc == 4)
			return runChangeChild(juce::File(_argv[2]), juce::File(_argv[3]));
		return fail("invalid child arguments");
	}

	const auto directory = juce::File::getCurrentWorkingDirectory()
		.getNonexistentChildFile("gearmulator-md-standalone-plusdrive-test", {}, false);
	const auto createResult = directory.createDirectory();
	if(createResult.failed())
	{
		std::fprintf(stderr,
			"mdStandalonePlusDrivePersistenceTest: could not create %s: %s\n",
			directory.getFullPathName().toRawUTF8(),
			createResult.getErrorMessage().toRawUTF8());
		return 1;
	}
	struct Cleanup final
	{
		juce::File directory;
		~Cleanup() { directory.deleteRecursively(); }
	} cleanup{directory};

	const auto imageA = makeImage(7, 0x5a);
	const auto imageB = makeImage(9, 0x27);
	if(!md::PlusDrive::validateStorage(imageA)
		|| !md::PlusDrive::validateStorage(imageB))
		return fail("test image was invalid");

	std::mutex operationMutex;
	std::mutex stateMutex;
	mdJucePlugin::StandalonePlusDrivePersistence::Snapshot live;
	live.hardwareEpoch = 1;
	live.generation = 1;
	live.dirty = true;
	live.data = imageA;
	const auto capture = [&](const bool _includeData,
		mdJucePlugin::StandalonePlusDrivePersistence::Snapshot& _snapshot)
	{
		std::lock_guard lock(stateMutex);
		_snapshot.hardwareEpoch = live.hardwareEpoch;
		_snapshot.generation = live.generation;
		_snapshot.dirty = live.dirty;
		if(_includeData)
			_snapshot.data = live.data;
		return true;
	};
	const auto acknowledge = [&](const uint64_t _epoch, const uint64_t _generation)
	{
		std::lock_guard lock(stateMutex);
		if(live.hardwareEpoch == _epoch && live.generation == _generation)
			live.dirty = false;
	};

	const auto file = directory.getChildFile("standalone.mdpd");
	{
		mdJucePlugin::StandalonePlusDrivePersistence owner(
			file, operationMutex, capture, acknowledge);
		const auto ownerStart = owner.start();
		if(!ownerStart.writable || ownerStart.hasInitialImage)
			return fail("new checkpoint did not acquire sole write ownership");

		mdJucePlugin::StandalonePlusDrivePersistence competitor(
			file, operationMutex, capture, acknowledge);
		const auto competitorStart = competitor.start();
		if(competitorStart.writable || competitor.ownsWriter())
			return fail("two instances acquired the standalone checkpoint");

		std::this_thread::sleep_for(std::chrono::milliseconds(1250));
		if(file.existsAsFile() || owner.authoritative())
			return fail("initial firmware dirtiness was mistaken for user-owned state");

		owner.requestFlush(true);
		if(!waitForImage(file, imageA) || !owner.authoritative())
			return fail("first debounced checkpoint was not committed");
		{
			std::lock_guard lock(stateMutex);
			live.generation = 2;
			live.dirty = true;
			live.data = imageB;
		}
		if(!waitForImage(file, imageB))
			return fail("dirty generation was not checkpointed");
		competitor.stop();
		owner.stop();
	}

	{
		mdJucePlugin::StandalonePlusDrivePersistence restored(
			file, operationMutex, capture, acknowledge);
		const auto start = restored.start();
		if(!start.writable || !start.hasInitialImage || start.initialImage != imageB
			|| !restored.authoritative())
			return fail("checkpoint did not restore as the standalone authority");
		restored.stop();
	}

	// Exercise the named OS lock with genuinely separate processes. A running
	// second instance stays read-only for its entire session; a newly launched
	// process can take ownership after the original writer exits.
	const auto executable = juce::File::getSpecialLocation(
		juce::File::currentExecutableFile).getFullPathName();
	const auto processFile = directory.getChildFile("process-owned.mdpd");
	const auto ownerReady = directory.getChildFile("owner-ready");
	const auto ownerRelease = directory.getChildFile("owner-release");
	juce::ChildProcess ownerChild;
	if(!startChild(ownerChild, childArguments({executable, "--owner",
		processFile.getFullPathName(), ownerReady.getFullPathName(),
		ownerRelease.getFullPathName()})))
		return fail("could not launch checkpoint owner process");
	if(!waitForFile(ownerReady) || !waitForImage(processFile, imageA))
	{
		ownerChild.kill();
		return fail("checkpoint owner process did not become ready");
	}
	juce::ChildProcess readerChild;
	if(!startChild(readerChild, childArguments({executable, "--probe-reader",
		processFile.getFullPathName()})) || !childSucceeded(readerChild))
	{
		ownerChild.kill();
		return fail("cross-process checkpoint exclusion failed");
	}
	if(!ownerRelease.replaceWithText("release") || !childSucceeded(ownerChild))
		return fail("checkpoint owner process did not exit cleanly");
	juce::ChildProcess writerChild;
	if(!startChild(writerChild, childArguments({executable, "--probe-writer",
		processFile.getFullPathName()})) || !childSucceeded(writerChild))
		return fail("released checkpoint was not available to a fresh process");

	// A forced termination inside the debounce interval may lose the newest
	// change, but it must leave the previous checkpoint byte-for-byte valid.
	const auto crashFile = directory.getChildFile("forced-termination.mdpd");
	if(!baseLib::filesystem::writeFileAtomic(
		crashFile.getFullPathName().toStdString(), imageA))
		return fail("could not prepare forced-termination checkpoint");
	const auto crashReady = directory.getChildFile("crash-ready");
	juce::ChildProcess crashChild;
	if(!startChild(crashChild, childArguments({executable, "--change",
		crashFile.getFullPathName(), crashReady.getFullPathName()}))
		|| !waitForFile(crashReady))
	{
		crashChild.kill();
		return fail("forced-termination child did not become ready");
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	if(!crashChild.kill() || !crashChild.waitForProcessToFinish(5000)
		|| !waitForImage(crashFile, imageA))
		return fail("forced termination damaged the prior checkpoint");

	// Once the same change remains stable past the debounce window, it must be
	// visible to another process even if the writer is then forcibly terminated.
	crashReady.deleteFile();
	juce::ChildProcess settledChild;
	if(!startChild(settledChild, childArguments({executable, "--change",
		crashFile.getFullPathName(), crashReady.getFullPathName()}))
		|| !waitForFile(crashReady) || !waitForImage(crashFile, imageB))
	{
		settledChild.kill();
		return fail("stable change exceeded the standalone debounce bound");
	}
	if(!settledChild.kill() || !settledChild.waitForProcessToFinish(5000)
		|| !waitForImage(crashFile, imageB))
		return fail("settled checkpoint did not survive forced termination");

	const auto corruptFile = directory.getChildFile("corrupt.mdpd");
	const std::vector<uint8_t> corrupt{1, 2, 3, 4};
	if(!baseLib::filesystem::writeFileAtomic(
		corruptFile.getFullPathName().toStdString(), corrupt))
		return fail("could not prepare corrupt checkpoint");
	{
		mdJucePlugin::StandalonePlusDrivePersistence recovery(
			corruptFile, operationMutex, capture, acknowledge);
		const auto start = recovery.start();
		if(!recovery.ownsWriter() || start.writable || start.hasInitialImage)
			return fail("invalid checkpoint was not preserved behind explicit recovery");
		recovery.allowReplacementAndFlush();
		if(!waitForImage(corruptFile, imageB))
			return fail("explicit recovery did not replace the invalid checkpoint");
		recovery.stop();
	}

	std::puts("mdStandalonePlusDrivePersistenceTest: PASS");
	return 0;
}
