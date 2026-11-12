#pragma once

#include "juce_core/juce_core.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mdJucePlugin
{
	class StandalonePlusDrivePersistence final
	{
	public:
		struct Snapshot
		{
			uint64_t hardwareEpoch = 0;
			uint64_t generation = 0;
			bool dirty = false;
			std::vector<uint8_t> data;
		};

		using Capture = std::function<bool(bool, Snapshot&)>;
		using Acknowledge = std::function<void(uint64_t, uint64_t)>;

		struct StartResult
		{
			std::vector<uint8_t> initialImage;
			bool hasInitialImage = false;
			bool writable = false;
		};

		StandalonePlusDrivePersistence(juce::File _file, std::mutex& _operationMutex,
			Capture _capture, Acknowledge _acknowledge);
		~StandalonePlusDrivePersistence();

		StartResult start();
		void stop();
		void requestFlush(bool _adoptCurrent = false);
		void allowReplacementAndFlush();
		void blockWrites(const juce::String& _reason);

		bool ownsWriter() const;
		bool writable() const;
		bool authoritative() const;
		juce::String status() const;
		const juce::File& file() const { return m_file; }

	private:
		void startThreadIfAllowed();
		void run();
		bool writeSnapshot(const Snapshot& _snapshot, const char* _failurePrefix);

		const juce::File m_file;
		std::mutex& m_operationMutex;
		const Capture m_capture;
		const Acknowledge m_acknowledge;
		mutable std::mutex m_mutex;
		std::condition_variable m_cv;
		std::thread m_thread;
		std::unique_ptr<juce::InterProcessLock> m_interprocessLock;
		juce::String m_status;
		bool m_started = false;
		bool m_writer = false;
		bool m_writeBlocked = false;
		bool m_stop = false;
		bool m_authoritative = false;
		bool m_hasInitialGeneration = false;
		uint64_t m_initialHardwareEpoch = 0;
		uint64_t m_initialGeneration = 0;
		uint64_t m_flushRequest = 0;
		uint64_t m_flushedRequest = 0;
	};
}
