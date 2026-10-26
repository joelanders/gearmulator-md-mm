#include "mdLib/mdsysexrecorder.h"

#include <cstdio>
#include <stdexcept>
#include <vector>

namespace
{
	using Bytes = std::vector<uint8_t>;

	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	// A checksummed user-data dump in the shape the importer validates.
	Bytes dump(const uint8_t _product, const uint8_t _command, const uint8_t _slot, const size_t _payload)
	{
		Bytes message{0xf0, 0, 0x20, 0x3c, _product, 0, _command, 1, 1, _slot};
		for(size_t i = 0; i < _payload; ++i)
			message.push_back(static_cast<uint8_t>((i * 7 + _slot) & 0x7f));
		unsigned sum = 0;
		for(size_t i = 9; i < message.size(); ++i)
			sum += message[i];
		const auto length = message.size() + 5 - 10;
		message.insert(message.end(), {uint8_t((sum >> 7) & 0x7f), uint8_t(sum & 0x7f),
			uint8_t((length >> 7) & 0x7f), uint8_t(length & 0x7f), 0xf7});
		return message;
	}

	synthLib::SMidiEvent event(const Bytes& _sysex)
	{
		synthLib::SMidiEvent e(synthLib::MidiEventSource::Device);
		e.sysex.assign(_sysex.begin(), _sysex.end());
		return e;
	}

	Bytes storage(const size_t _capacity)
	{
		Bytes bytes;
		bytes.reserve(_capacity);
		return bytes;
	}

	void testFiltersToImportableUserData()
	{
		md::MidiSysexRecorder recorder(md::MachineModel::Monomachine);
		auto buffer = storage(1 << 16);
		require(recorder.start(buffer, 0), "start failed");
		require(!recorder.start(buffer, 0), "second start accepted");

		const auto kit = dump(3, 0x52, 4, 40);
		const auto song = dump(3, 0x69, 2, 20);
		const std::vector<synthLib::SMidiEvent> events{
			event({0xf0, 0x7e, 0x00, 0x7f, 0x00, 0xf7}),              // handshake, host-side response
			event(kit),
			event({0xf0, 0, 0x20, 0x3c, 0, 0, 0x10, 0, 0xf7}),        // TurboMIDI negotiation
			event({0xf0, 0, 0x20, 0x3c, 3, 0, 0x72, 1, 0, 0xf7}),     // status reply
			event(dump(2, 0x52, 0, 20)),                              // Machinedrum kit
			event({0xf0, 0x7e, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf7}), // SDS on MM
			synthLib::SMidiEvent(synthLib::MidiEventSource::Device, 0xf8),
			event(song),
		};
		recorder.capture(events, 1, 0);

		const auto& status = recorder.status();
		require(status.recording && status.messages == 2 && status.skipped == 4 && !status.truncated,
			"unexpected filter counts");

		Bytes recorded;
		md::MidiSysexRecordingStatus result;
		require(recorder.stop(recorded, result), "stop failed");
		require(!result.recording && result.messages == 2, "stop status");
		Bytes expected = kit;
		expected.insert(expected.end(), song.begin(), song.end());
		require(recorded == expected, "recording differs from the dumps");
		require(md::parseMidiSysexFile(recorded, md::MachineModel::Monomachine) == md::MidiSysexStreamValidation::Valid,
			"recording is not importable");
		require(!recorder.status().recording && !recorder.stop(recorded, result), "recorder still active after stop");
	}

	void testMachinedrumKeepsSamplesAndNames()
	{
		md::MidiSysexRecorder recorder(md::MachineModel::Machinedrum);
		auto buffer = storage(1024);
		require(recorder.start(buffer, 0), "start failed");
		const Bytes header{0xf0, 0x7e, 0x00, 0x01, 0x00, 0x00, 16, 1, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0x7f, 0xf7};
		const Bytes name{0xf0, 0, 0x20, 0x3c, 2, 0, 0x73, 3, 'K', 'I', 'C', 'K', 0xf7};
		const Bytes ack{0xf0, 0x7e, 0x00, 0x7f, 0x00, 0xf7};
		recorder.capture({event(header), event(name), event(ack)}, 0, 0);
		require(recorder.status().messages == 2 && recorder.status().skipped == 1, "MD sample filter");
	}

	void testCapacityAndOverflowMarkTruncated()
	{
		md::MidiSysexRecorder recorder(md::MachineModel::Monomachine);
		const auto kit = dump(3, 0x52, 0, 100);
		auto buffer = storage(kit.size() + 10);
		require(recorder.start(buffer, 5), "start failed");
		recorder.capture({event(kit), event(kit)}, 0, 5);
		require(recorder.status().messages == 1 && recorder.status().truncated, "capacity limit not reported");
		require(recorder.status().bytes == kit.size(), "partial message stored");

		md::MidiSysexRecorder overflow(md::MachineModel::Monomachine);
		auto other = storage(1024);
		require(overflow.start(other, 5), "start failed");
		overflow.capture({}, 0, 6);
		require(overflow.status().truncated, "UART overflow not reported");
	}

	void testInterruptStopsCapture()
	{
		md::MidiSysexRecorder recorder(md::MachineModel::Monomachine);
		auto buffer = storage(4096);
		require(recorder.start(buffer, 0), "start failed");
		const auto kit = dump(3, 0x52, 0, 20);
		recorder.capture({event(kit)}, 0, 0);
		recorder.interrupt();
		require(!recorder.capturing() && recorder.status().interrupted, "interrupt ignored");
		recorder.capture({event(kit)}, 0, 0);
		require(recorder.status().messages == 1, "captured output from a replaced machine");
		Bytes recorded;
		md::MidiSysexRecordingStatus result;
		require(recorder.stop(recorded, result) && result.interrupted && recorded == kit, "interrupted stop");
	}
}

int main()
{
	try
	{
		testFiltersToImportableUserData();
		testMachinedrumKeepsSamplesAndNames();
		testCapacityAndOverflowMarkTruncated();
		testInterruptStopsCapture();
		std::puts("SysEx recorder tests passed");
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "SysEx recorder test failed: %s\n", _error.what());
		return 1;
	}
}
