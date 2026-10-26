#include "mdLib/mdsysexdumpexport.h"
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

	std::vector<synthLib::SMidiEvent> reply(const Bytes& _sysex)
	{
		return {event(_sysex)};
	}

	void testCurrentKitExportSequence()
	{
		md::SysexDumpExport job(md::MachineModel::Machinedrum);
		require(job.start(md::SysexDumpScope::CurrentKit, true), "start failed");
		require(!job.start(md::SysexDumpScope::AllUserData, false), "second start accepted");

		auto request = job.service({}, 0);
		require(request && (*request)[6] == 0x70 && (*request)[7] == 0x02, "expected kit status request");
		require(!job.service({}, 0), "re-sent while waiting");
		// Other traffic, including a pattern status reply, is not the answer.
		require(!job.service(reply({0xf0, 0, 0x20, 0x3c, 2, 0, 0x72, 0x04, 9, 0xf7}), 0), "wrong status accepted");

		request = job.service(reply({0xf0, 0, 0x20, 0x3c, 2, 0, 0x72, 0x02, 7, 0xf7}), 0);
		require(request && (*request)[6] == 0x59 && (*request)[7] == 7, "expected kit save to slot 7");
		require(job.progress().slot == 7, "slot not reported");
		request = job.service({}, 0);
		require(request && (*request)[6] == 0x53 && (*request)[7] == 7, "expected kit 7 request");

		require(!job.service(reply(dump(2, 0x52, 6, 20)), 0), "wrong slot accepted");
		require(job.running(), "finished early");
		require(!job.service(reply(dump(2, 0x52, 7, 20)), 0) && !job.running(), "kit dump not accepted");
		require(job.progress().state == md::SysexDumpExportState::Complete
			&& job.progress().done == 1 && job.progress().total == 1, "unexpected final progress");
	}

	void testAllUserDataPlanAndTimeout()
	{
		md::SysexDumpExport mm(md::MachineModel::Monomachine);
		require(mm.start(md::SysexDumpScope::AllUserData, false), "start failed");
		require(mm.progress().total == 8 + 128 + 128 + 24, "MM plan size");
		const auto first = mm.service({}, 0);
		require(first && (*first)[4] == 3 && (*first)[6] == 0x51 && (*first)[7] == 0, "expected MM global 0 request");
		mm.advance(md::SysexDumpExport::g_replyTimeoutSamples - 1);
		require(mm.running(), "timed out early");
		mm.advance(1);
		require(mm.progress().state == md::SysexDumpExportState::Failed, "missing reply did not time out");

		md::SysexDumpExport md(md::MachineModel::Machinedrum);
		require(md.start(md::SysexDumpScope::AllUserData, false) && md.progress().total == 8 + 64 + 128 + 32, "MD plan size");
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
		testCurrentKitExportSequence();
		testAllUserDataPlanAndTimeout();
		std::puts("SysEx recorder tests passed");
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "SysEx recorder test failed: %s\n", _error.what());
		return 1;
	}
}
