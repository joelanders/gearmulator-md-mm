#pragma once

#include <cstdint>

namespace md::automation
{
	// Protocol-only ordering state for one firmware dump resource. A timed-out
	// dump is never requested again directly: a fresh status response first acts
	// as a FIFO barrier, so any late dump from the retired request is rejected.
	// Time is supplied by the caller, keeping this state machine deterministic and
	// independent of JUCE, threads, and wall-clock scheduling.
	class DumpRequestTracker
	{
	public:
		enum class Phase : uint8_t
		{
			AwaitingStatus,
			AwaitingDump,
			Ready
		};

		struct StatusResult
		{
			bool accepted = false;
			bool requestDump = false;
			bool selectionChanged = false;
		};

		explicit DumpRequestTracker(bool _refreshSameSlot)
			: m_refreshSameSlot(_refreshSameSlot) {}

		void reset();
		bool statusRequestDue(uint64_t _now, uint64_t _retryInterval) const;
		bool canPollStatus() const;
		void statusRequestSent(uint64_t _now);
		bool recoverTimedOutStatus(uint64_t _now, uint64_t _retryInterval);
		StatusResult observeStatus(uint8_t _slot);
		void dumpRequestSent(uint64_t _now);
		bool acceptDump(uint8_t _slot);
		bool recoverTimedOutDump(uint64_t _now, uint64_t _retryInterval);

		Phase phase() const { return m_phase; }
		bool ready() const { return m_phase == Phase::Ready; }
		uint8_t currentSlot() const { return m_currentSlot; }

	private:
		const bool m_refreshSameSlot;
		Phase m_phase = Phase::AwaitingStatus;
		uint8_t m_currentSlot = 0xff;
		bool m_statusOutstanding = false;
		bool m_dumpOutstanding = false;
		uint64_t m_statusRequestTime = 0;
		uint64_t m_dumpRequestTime = 0;
	};
}
