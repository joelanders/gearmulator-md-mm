#include "mdautomationsync.h"

namespace md::automation
{
	void DumpRequestTracker::reset()
	{
		m_phase = Phase::AwaitingStatus;
		m_currentSlot = 0xff;
		m_statusOutstanding = false;
		m_dumpOutstanding = false;
		m_statusRequestTime = 0;
		m_dumpRequestTime = 0;
	}

	bool DumpRequestTracker::statusRequestDue(const uint64_t _now,
		const uint64_t _retryInterval) const
	{
		if(m_phase != Phase::AwaitingStatus)
			return false;
		return !m_statusOutstanding
			|| _now - m_statusRequestTime >= _retryInterval;
	}

	bool DumpRequestTracker::canPollStatus() const
	{
		return m_phase == Phase::Ready && !m_statusOutstanding;
	}

	void DumpRequestTracker::statusRequestSent(const uint64_t _now)
	{
		m_statusOutstanding = true;
		m_statusRequestTime = _now;
	}

	bool DumpRequestTracker::recoverTimedOutStatus(const uint64_t _now,
		const uint64_t _retryInterval)
	{
		if(!m_statusOutstanding
			|| _now - m_statusRequestTime < _retryInterval)
			return false;
		m_statusOutstanding = false;
		return true;
	}

	DumpRequestTracker::StatusResult DumpRequestTracker::observeStatus(
		const uint8_t _slot)
	{
		if(!m_statusOutstanding)
			return {};
		m_statusOutstanding = false;
		const auto selectionChanged = m_currentSlot != _slot;
		const auto needsDump = m_phase == Phase::AwaitingStatus
			|| selectionChanged
			|| (m_phase == Phase::Ready && m_refreshSameSlot);
		m_currentSlot = _slot;
		if(needsDump)
		{
			m_phase = Phase::AwaitingDump;
			m_dumpOutstanding = false;
			return {true, true, selectionChanged};
		}
		return {true, false, selectionChanged};
	}

	void DumpRequestTracker::dumpRequestSent(const uint64_t _now)
	{
		if(m_phase != Phase::AwaitingDump)
			return;
		m_dumpOutstanding = true;
		m_dumpRequestTime = _now;
	}

	bool DumpRequestTracker::acceptDump(const uint8_t _slot)
	{
		if(m_phase != Phase::AwaitingDump || !m_dumpOutstanding
			|| _slot != m_currentSlot)
			return false;
		m_dumpOutstanding = false;
		m_phase = Phase::Ready;
		return true;
	}

	bool DumpRequestTracker::recoverTimedOutDump(const uint64_t _now,
		const uint64_t _retryInterval)
	{
		if(m_phase != Phase::AwaitingDump || !m_dumpOutstanding
			|| _now - m_dumpRequestTime < _retryInterval)
			return false;
		m_phase = Phase::AwaitingStatus;
		m_statusOutstanding = false;
		m_dumpOutstanding = false;
		return true;
	}
}
