#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mdsysexfile.h"
#include "mdtypes.h"

#include "synthLib/midiTypes.h"

namespace md
{
	// True for one complete machine-originated SysEx message that "Send SysEx
	// File" would accept for this model. TurboMIDI negotiation, SDS handshakes,
	// status replies and requests are skipped, so a finished recording is the
	// same kind of file the importer reads.
	inline bool isRecordableMidiSysex(const uint8_t* _b, const size_t _size, const MachineModel _model)
	{
		if(_size < 5 || _b[0] != 0xf0 || _b[_size - 1] != 0xf7)
			return false;
		if(_b[1] == 0x7e)
			return _model == MachineModel::Machinedrum && (_b[3] == 1 || _b[3] == 2);
		if(_size < 8 || _b[1] != 0 || _b[2] != 0x20 || _b[3] != 0x3c)
			return false;
		const uint8_t product = _model == MachineModel::Monomachine ? 3 : 2;
		if(_b[4] != product)
			return false;
		const auto command = _b[6];
		return command == 0x50 || command == 0x52 || command == 0x67 || command == 0x69
			|| (product == 3 && command == 0x5d)
			|| (product == 2 && command == 0x73);
	}

	struct MidiSysexRecordingStatus
	{
		bool recording = false;
		size_t messages = 0;
		size_t bytes = 0;
		size_t skipped = 0;
		// Bytes were lost: the 8 MiB limit was reached, the emulated UART output
		// overflowed, or the machine was replaced while recording.
		bool truncated = false;
		bool interrupted = false;
	};

	// Captures user-data dumps the emulated machine sends on its MIDI output.
	// Every call requires the owning Plugin/processor lock. Storage is supplied
	// and returned by the caller so capture never allocates or frees on audio time.
	class MidiSysexRecorder
	{
	public:
		explicit MidiSysexRecorder(const MachineModel _model) : m_model(_model) {}

		bool start(std::vector<uint8_t>& _storage, const uint64_t _txOverflowCount)
		{
			if(m_status.recording)
				return false;
			_storage.clear();
			m_bytes.swap(_storage);
			m_status = {};
			m_status.recording = true;
			m_txOverflowBaseline = _txOverflowCount;
			return true;
		}

		// _from indexes the first event the machine produced in this block. Host
		// MIDI responses earlier in the same vector are not machine output.
		void capture(const std::vector<synthLib::SMidiEvent>& _events, const size_t _from,
			const uint64_t _txOverflowCount)
		{
			if(!capturing())
				return;
			if(_txOverflowCount != m_txOverflowBaseline)
				m_status.truncated = true;
			for(size_t i = _from; i < _events.size(); ++i)
			{
				const auto& sysex = _events[i].sysex;
				if(sysex.empty())
					continue;
				if(!isRecordableMidiSysex(sysex.data(), sysex.size(), m_model))
				{
					++m_status.skipped;
					continue;
				}
				if(m_bytes.size() + sysex.size() > m_bytes.capacity())
				{
					m_status.truncated = true;
					continue;
				}
				m_bytes.insert(m_bytes.end(), sysex.begin(), sysex.end());
				++m_status.messages;
			}
			m_status.bytes = m_bytes.size();
		}

		// The machine that produced the first part of the recording is gone; keep
		// what was captured but stop appending output from its replacement.
		void interrupt()
		{
			if(!m_status.recording)
				return;
			m_status.interrupted = true;
			m_status.truncated = true;
		}

		bool stop(std::vector<uint8_t>& _bytes, MidiSysexRecordingStatus& _status)
		{
			if(!m_status.recording)
				return false;
			_status = m_status;
			_status.recording = false;
			m_bytes.swap(_bytes);
			m_bytes = {};
			m_status = {};
			return true;
		}

		bool capturing() const { return m_status.recording && !m_status.interrupted; }
		const MidiSysexRecordingStatus& status() const { return m_status; }

	private:
		const MachineModel m_model;
		std::vector<uint8_t> m_bytes;
		MidiSysexRecordingStatus m_status;
		uint64_t m_txOverflowBaseline = 0;
	};
}
