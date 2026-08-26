#include "midiBufferParser.h"

#include "midiTypes.h"

#include <iterator>

namespace synthLib
{
	MidiBufferParser::MidiBufferParser(const MidiEventSource _source)
	{
		m_pendingEvent.source = _source;
	}

	void MidiBufferParser::write(const std::vector<uint8_t>& _data)
	{
		for (const auto d : _data)
			write(d);
	}

	void MidiBufferParser::write(uint8_t d)
	{
		// Realtime messages may be interleaved anywhere, including between the data
		// bytes of a channel message or inside SysEx. They do not disturb either
		// parser state.
		if(d >= M_TIMINGCLOCK)
		{
			m_midiEvents.emplace_back(m_pendingEvent.source, d);
			return;
		}

		if(d == synthLib::M_STARTOFSYSEX)
		{
			m_pendingEventLen = 0;
			flushSysex();
			m_sysex = true;
			m_sysexBuffer.push_back(d);
			return;
		}

		if(m_sysex)
		{
			if(d == synthLib::M_ENDOFSYSEX)
			{
				flushSysex();
				return;
			}
			if(d < 0x80)
			{
				m_sysexBuffer.push_back(d);
				return;
			}
			flushSysex();	// aborted sysex
		}

		// Any non-realtime status supersedes an incomplete short message. This is
		// the MIDI resynchronisation boundary after a lost/truncated data byte.
		if((d & 0x80) != 0)
			m_pendingEventLen = 0;

		if(m_pendingEventLen == 0)
		{
			m_pendingEvent.a = d;
			m_pendingEvent.b = 0;
			m_pendingEvent.c = 0;
			m_pendingEvent.offset = 0;
			m_pendingEvent.sysex.clear();
		}
		else if(m_pendingEventLen == 1)
			m_pendingEvent.b = d;
		else if(m_pendingEventLen == 2)
			m_pendingEvent.c = d;

		++m_pendingEventLen;

		if(lengthFromStatusByte(m_pendingEvent.a) == m_pendingEventLen)
			flushEvent();
	}

	void MidiBufferParser::getEvents(std::vector<synthLib::SMidiEvent>& _events)
	{
		_events.insert(_events.end(),
			std::make_move_iterator(m_midiEvents.begin()),
			std::make_move_iterator(m_midiEvents.end()));
		m_midiEvents.clear();
	}

	void MidiBufferParser::discardPartialMessage()
	{
		m_pendingEventLen = 0;
		m_sysex = false;
		m_sysexBuffer.clear();
	}

	void MidiBufferParser::flushSysex()
	{
		m_sysex = false;

		if(m_sysexBuffer.empty())
			return;

		SMidiEvent ev(m_pendingEvent.source);
		transferSysex(ev.sysex, m_sysexBuffer);

		if(ev.sysex.back() != M_ENDOFSYSEX)
			ev.sysex.push_back(M_ENDOFSYSEX);

		m_midiEvents.push_back(ev);
		m_sysexBuffer.clear();
	}

	void MidiBufferParser::flushEvent()
	{
		m_midiEvents.push_back(m_pendingEvent);
		m_pendingEventLen = 0;
	}
}
