#include "mdStandalonePlusDrivePersistence.h"

#include "mdStorageImage.h"

#include <chrono>
#include <utility>

namespace
{
	std::mutex g_standalonePlusDriveOwnerMutex;
	const mdJucePlugin::StandalonePlusDrivePersistence* g_standalonePlusDriveOwner = nullptr;

	bool claimStandalonePlusDrive(
		const mdJucePlugin::StandalonePlusDrivePersistence* const _owner)
	{
		std::lock_guard lock(g_standalonePlusDriveOwnerMutex);
		if(g_standalonePlusDriveOwner && g_standalonePlusDriveOwner != _owner)
			return false;
		g_standalonePlusDriveOwner = _owner;
		return true;
	}

	void releaseStandalonePlusDrive(
		const mdJucePlugin::StandalonePlusDrivePersistence* const _owner)
	{
		std::lock_guard lock(g_standalonePlusDriveOwnerMutex);
		if(g_standalonePlusDriveOwner == _owner)
			g_standalonePlusDriveOwner = nullptr;
	}

	juce::String lockName(const juce::File& _file)
	{
		return "gearmulator-md-standalone-plusdrive-"
			+ juce::String::toHexString(_file.getFullPathName().hashCode64());
	}
}

namespace mdJucePlugin
{
	StandalonePlusDrivePersistence::StandalonePlusDrivePersistence(
		juce::File _file, std::mutex& _operationMutex, Capture _capture,
		Acknowledge _acknowledge)
		: m_file(std::move(_file))
		, m_operationMutex(_operationMutex)
		, m_capture(std::move(_capture))
		, m_acknowledge(std::move(_acknowledge))
	{
	}

	StandalonePlusDrivePersistence::~StandalonePlusDrivePersistence()
	{
		stop();
	}

	StandalonePlusDrivePersistence::StartResult
	StandalonePlusDrivePersistence::start()
	{
		StartResult result;
		{
			std::lock_guard lock(m_mutex);
			if(m_started)
			{
				result.writable = m_writer && !m_writeBlocked;
				return result;
			}
			m_started = true;
		}
		Snapshot initialMetadata;
		if(m_capture(false, initialMetadata))
		{
			m_hasInitialGeneration = true;
			m_initialHardwareEpoch = initialMetadata.hardwareEpoch;
			m_initialGeneration = initialMetadata.generation;
		}

		const bool inProcessOwner = claimStandalonePlusDrive(this);
		if(inProcessOwner)
		{
			m_interprocessLock = std::make_unique<juce::InterProcessLock>(lockName(m_file));
			m_writer = m_interprocessLock->enter(0);
			if(!m_writer)
			{
				m_interprocessLock.reset();
				releaseStandalonePlusDrive(this);
			}
		}

		juce::String readError;
		if(m_file.existsAsFile())
		{
			result.hasInitialImage = storageImage::readPlusDrive(
				m_file, result.initialImage, readError);
			m_authoritative = result.hasInitialImage;
			if(!result.hasInitialImage && m_writer)
				m_writeBlocked = true;
		}

		{
			std::lock_guard lock(m_mutex);
			if(result.hasInitialImage)
				m_status = m_writer
					? "Standalone +Drive checkpoint active: " + m_file.getFullPathName()
					: "Standalone +Drive is read-only for this session; restart after closing "
						"the other instance to persist changes. Checkpoint: "
						+ m_file.getFullPathName();
			else if(m_file.existsAsFile())
				m_status = m_writer
					? "Standalone +Drive checkpoint is invalid and was preserved. Import a valid image to replace it. "
						+ readError
					: "Standalone +Drive checkpoint is unavailable and another instance owns it. "
						+ readError;
			else
				m_status = m_writer
					? "Standalone +Drive checkpoint will be created at "
						+ m_file.getFullPathName()
					: "Standalone +Drive is temporary for this session; restart after closing "
						"the other instance to persist changes. Checkpoint: "
						+ m_file.getFullPathName();
		}

		result.writable = m_writer && !m_writeBlocked;
		startThreadIfAllowed();
		return result;
	}

	void StandalonePlusDrivePersistence::stop()
	{
		{
			std::lock_guard lock(m_mutex);
			if(!m_started)
				return;
			m_stop = true;
		}
		m_cv.notify_all();
		if(m_thread.joinable())
			m_thread.join();
		if(m_interprocessLock)
		{
			m_interprocessLock->exit();
			m_interprocessLock.reset();
		}
		releaseStandalonePlusDrive(this);
		std::lock_guard lock(m_mutex);
		m_started = false;
		m_writer = false;
	}

	void StandalonePlusDrivePersistence::requestFlush(const bool _adoptCurrent)
	{
		{
			std::lock_guard lock(m_mutex);
			if(_adoptCurrent)
				m_authoritative = true;
			++m_flushRequest;
		}
		m_cv.notify_all();
	}

	void StandalonePlusDrivePersistence::allowReplacementAndFlush()
	{
		{
			std::lock_guard lock(m_mutex);
			m_writeBlocked = false;
			m_authoritative = true;
			++m_flushRequest;
		}
		startThreadIfAllowed();
		m_cv.notify_all();
	}

	void StandalonePlusDrivePersistence::blockWrites(const juce::String& _reason)
	{
		{
			std::lock_guard lock(m_mutex);
			m_writeBlocked = true;
			m_stop = true;
			m_status = _reason;
		}
		m_cv.notify_all();
		if(m_thread.joinable())
			m_thread.join();
		std::lock_guard lock(m_mutex);
		m_stop = false;
	}

	bool StandalonePlusDrivePersistence::ownsWriter() const
	{
		std::lock_guard lock(m_mutex);
		return m_writer;
	}

	bool StandalonePlusDrivePersistence::writable() const
	{
		std::lock_guard lock(m_mutex);
		return m_writer && !m_writeBlocked;
	}

	bool StandalonePlusDrivePersistence::authoritative() const
	{
		std::lock_guard lock(m_mutex);
		return m_authoritative;
	}

	juce::String StandalonePlusDrivePersistence::status() const
	{
		std::lock_guard lock(m_mutex);
		return m_status;
	}

	void StandalonePlusDrivePersistence::startThreadIfAllowed()
	{
		std::lock_guard lock(m_mutex);
		if(m_writer && !m_writeBlocked && !m_thread.joinable())
		{
			m_stop = false;
			m_thread = std::thread([this] { run(); });
		}
	}

	bool StandalonePlusDrivePersistence::writeSnapshot(
		const Snapshot& _snapshot, const char* const _failurePrefix)
	{
		juce::String error;
		const bool written = storageImage::writePlusDriveAtomically(
			m_file, _snapshot.data, error);
		{
			std::lock_guard lock(m_mutex);
			m_status = written
				? "Standalone +Drive checkpoint is current: " + m_file.getFullPathName()
				: juce::String(_failurePrefix) + error;
			if(written)
				m_authoritative = true;
		}
		if(written)
			m_acknowledge(_snapshot.hardwareEpoch, _snapshot.generation);
		return written;
	}

	void StandalonePlusDrivePersistence::run()
	{
		using Clock = std::chrono::steady_clock;
		uint64_t observedEpoch = 0;
		uint64_t observedGeneration = 0;
		auto flushAfter = Clock::now();
		while(true)
		{
			uint64_t flushRequest = 0;
			uint64_t flushedRequest = 0;
			bool stopping = false;
			bool writeBlocked = false;
			bool authoritative = false;
			{
				std::unique_lock lock(m_mutex);
				m_cv.wait_for(lock, std::chrono::milliseconds(250),
					[this] { return m_stop; });
				stopping = m_stop;
				writeBlocked = m_writeBlocked;
				authoritative = m_authoritative;
				flushRequest = m_flushRequest;
				flushedRequest = m_flushedRequest;
			}
			if(writeBlocked)
				return;

			std::unique_lock operationLock(m_operationMutex, std::defer_lock);
			if(stopping)
				operationLock.lock();
			else if(!operationLock.try_lock())
				continue;

			Snapshot metadata;
			if(!m_capture(false, metadata))
			{
				if(stopping)
					return;
				continue;
			}

			const bool forced = flushRequest != flushedRequest;
			const bool changedSinceStart = m_hasInitialGeneration
				&& (metadata.hardwareEpoch != m_initialHardwareEpoch
					|| metadata.generation != m_initialGeneration);
			if(!authoritative && !forced && !changedSinceStart)
			{
				if(stopping)
					return;
				continue;
			}
			if(!authoritative)
			{
				std::lock_guard lock(m_mutex);
				m_authoritative = true;
			}
			if(stopping)
			{
				Snapshot snapshot;
				if(m_capture(true, snapshot)
					&& writeSnapshot(snapshot, "Final standalone +Drive checkpoint failed: "))
				{
					std::lock_guard lock(m_mutex);
					m_flushedRequest = flushRequest;
				}
				return;
			}
			if(!metadata.dirty && !forced)
				continue;

			const auto now = Clock::now();
			if(metadata.hardwareEpoch != observedEpoch
				|| metadata.generation != observedGeneration)
			{
				observedEpoch = metadata.hardwareEpoch;
				observedGeneration = metadata.generation;
				flushAfter = now + std::chrono::seconds(1);
				continue;
			}
			if(now < flushAfter)
				continue;

			Snapshot snapshot;
			if(!m_capture(true, snapshot)
				|| snapshot.hardwareEpoch != metadata.hardwareEpoch
				|| snapshot.generation != metadata.generation)
				continue;
			const bool written = writeSnapshot(snapshot,
				"Standalone +Drive checkpoint failed: ");
			if(written)
			{
				std::lock_guard lock(m_mutex);
				if(flushRequest > m_flushedRequest)
					m_flushedRequest = flushRequest;
			}
			flushAfter = Clock::now() + (written
				? std::chrono::seconds(1) : std::chrono::seconds(2));
		}
	}
}
