#include "mdStandalonePlusDrivePersistence.h"

#include "mdLib/mdplusdrive.h"

#include "baseLib/filesystem.h"

#include <chrono>
#include <cstdio>
#include <mutex>
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
		for(int attempt = 0; attempt < 80; ++attempt)
		{
			std::vector<uint8_t> actual;
			if(baseLib::filesystem::readFile(
				actual, _file.getFullPathName().toStdString()) && actual == _expected)
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
}

int main()
{
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
