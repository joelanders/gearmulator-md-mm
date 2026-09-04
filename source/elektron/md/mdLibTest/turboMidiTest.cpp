#include "mdLib/mdturbomidi.h"

#include <cstdint>
#include <atomic>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <thread>
#include <vector>

namespace
{
	constexpr uint64_t g_testClockHz = 1000;

	class MidiSink final : public md::MidiByteSink
	{
	public:
		bool tryWriteMidiByte(const uint8_t _byte) override
		{
			if(bytes.size() >= acceptLimit)
				return false;
			bytes.push_back(_byte);
			return true;
		}
		size_t queuedMidiByteCount() const override { return 0; }
		std::vector<uint8_t> bytes;
		size_t acceptLimit = std::numeric_limits<size_t>::max();
	};

	bool check(const bool _condition, const char* const _message)
	{
		if(_condition)
			return true;
		std::cerr << _message << '\n';
		return false;
	}

	std::vector<uint8_t> turboMessage(const uint8_t _command,
		const std::initializer_list<uint8_t> _payload = {})
	{
		std::vector<uint8_t> result{0xf0, 0x00, 0x20, 0x3c, 0x00, 0x00, _command};
		result.insert(result.end(), _payload.begin(), _payload.end());
		result.push_back(0xf7);
		return result;
	}

	void reply(md::TurboMidiTransfer& _transfer, const uint8_t _command,
		const std::initializer_list<uint8_t> _payload = {})
	{
		for(const auto byte : turboMessage(_command, _payload))
			_transfer.observeTransmitByte(byte);
	}

	bool testTimeoutFallback()
	{
		md::TurboMidiTransfer transfer(g_testClockHz);
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x01, 0xf7});
		if(!check(prepared.has_value(), "valid SysEx was rejected")
			|| !check(transfer.start(*prepared, 0), "transfer did not start"))
			return false;

		transfer.service(g_testClockHz, true, sink);
		if(!check(sink.bytes == turboMessage(0x10), "capability request is malformed"))
			return false;
		transfer.service(g_testClockHz + 1, true, sink);
		const auto progress = transfer.progress();
		return check(progress.state == md::MidiSysexTransferState::Complete,
			"fallback transfer did not complete")
			&& check(progress.sent == 3, "fallback byte count is wrong")
			&& check(progress.fallbackReason
				== md::MidiTurboFallbackReason::CapabilityRequestTimedOut,
				"fallback reason is wrong");
	}

	bool testDocumentedHandshake()
	{
		md::TurboMidiTransfer transfer(g_testClockHz);
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x02, 0xf7});
		if(!prepared || !transfer.start(*prepared, 0))
			return check(false, "TurboMIDI transfer did not start");

		transfer.service(g_testClockHz, true, sink);
		reply(transfer, 0x11, {0x7f, 0x01, 0x7f, 0x01});
		transfer.service(g_testClockHz, true, sink);
		reply(transfer, 0x13);
		transfer.service(g_testClockHz, true, sink);
		reply(transfer, 0x15,
			{0x55, 0x55, 0x55, 0x55, 0x00, 0x00, 0x00, 0x00});
		transfer.service(g_testClockHz, true, sink);
		reply(transfer, 0x17);
		transfer.service(g_testClockHz, true, sink);

		const auto progress = transfer.progress();
		return check(progress.state == md::MidiSysexTransferState::Complete,
			"TurboMIDI transfer did not complete")
			&& check(progress.sent == 3, "TurboMIDI byte count is wrong")
			&& check(progress.fallbackReason == md::MidiTurboFallbackReason::None,
				"TurboMIDI unexpectedly fell back");
	}

	bool testCancellationAndRetirement()
	{
		md::TurboMidiTransfer transfer(g_testClockHz);
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x01, 0xf7});
		if(!prepared || !transfer.start(*prepared, 0))
			return check(false, "cancellation transfer did not start");

		transfer.service(g_testClockHz, true, sink);
		sink.acceptLimit = sink.bytes.size() + 2;
		transfer.service(g_testClockHz + 1, true, sink);
		if(!check(transfer.progress().sent == 2,
			"cancellation fixture did not stop inside the payload"))
			return false;

		std::vector<uint8_t> retired;
		if(!check(transfer.cancel(retired), "active transfer was not cancelled")
			|| !check(retired == std::vector<uint8_t>({0xf0, 0x01, 0xf7}),
				"cancel did not hand payload storage to the caller")
			|| !check(transfer.progress().state
				== md::MidiSysexTransferState::Cancelling,
				"cancel did not retain MIDI-wire ownership"))
			return false;

		sink.acceptLimit = std::numeric_limits<size_t>::max();
		transfer.service(g_testClockHz, true, sink);
		return check(transfer.progress().state
				== md::MidiSysexTransferState::Cancelled,
			"cancel did not drain to a terminal state")
			&& check(sink.bytes.size() >= 3
				&& sink.bytes[sink.bytes.size() - 3] == 0xf0
				&& sink.bytes[sink.bytes.size() - 2] == 0x01
				&& sink.bytes.back() == 0xf7,
				"cancel did not terminate the partial SysEx message");
	}

	bool testCompletedPayloadRetirement()
	{
		md::TurboMidiTransfer transfer(g_testClockHz);
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x03, 0xf7});
		if(!prepared || !transfer.start(*prepared, 0))
			return check(false, "retirement transfer did not start");
		transfer.service(g_testClockHz, true, sink);
		transfer.service(g_testClockHz + 1, true, sink);

		std::vector<uint8_t> retired;
		return check(transfer.progress().state
				== md::MidiSysexTransferState::Complete,
			"retirement fixture did not complete")
			&& check(transfer.retirePayload(retired),
				"completed payload was not retired")
			&& check(retired == std::vector<uint8_t>({0xf0, 0x03, 0xf7}),
				"retired payload was corrupted")
			&& check(!transfer.retirePayload(retired),
				"payload retirement was not single-owner");
	}

	bool testPausedServiceResumes()
	{
		md::TurboMidiTransfer transfer(g_testClockHz);
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x04, 0xf7});
		if(!prepared || !transfer.start(*prepared, 0))
			return check(false, "paused transfer did not start");

		const auto paused = transfer.progress();
		// A suspended host makes no service calls. Control-plane observation must
		// remain safe and must not advance or discard the queued operation.
		for(size_t i = 0; i < 1000; ++i)
		{
			const auto observed = transfer.progress();
			if(observed.state != paused.state || observed.sent != paused.sent)
				return check(false, "paused transfer advanced without processing");
		}
		transfer.service(g_testClockHz, true, sink);
		transfer.service(g_testClockHz + 1, true, sink);
		const auto resumed = transfer.progress();
		return check(resumed.state == md::MidiSysexTransferState::Complete,
			"paused transfer did not resume")
			&& check(resumed.sent == resumed.total,
				"resumed transfer lost payload bytes");
	}

	bool testValidation()
	{
		using md::MidiSysexStreamValidation;
		const std::vector<uint8_t> mmOsUpdate{
			0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x7e, 0x00, 0xf7};
		const std::vector<uint8_t> mdRequest{
			0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x53, 0x01, 0xf7};
		const std::vector<uint8_t> validDump{
			0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x52, 0x01, 0x01,
			0x01, 0x00, 0x01, 0x00, 0x05, 0xf7};
		const std::vector<uint8_t> validDigiProFooter{
			0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x5d, 0x01, 0x01,
			0x21, 0x02, 0x00, 0x02, 0x00, 0x06, 0xf7};
		if(!check(md::validateMidiSysexStream(validDump, md::MachineModel::Machinedrum)
				== MidiSysexStreamValidation::Valid, "MD data was rejected")
			|| !check(md::validateMidiSysexStream(validDigiProFooter,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::Valid,
				"MM data was rejected")
			|| !check(md::validateMidiSysexStream(validDump,
				md::MachineModel::Monomachine)
				== MidiSysexStreamValidation::WrongModel, "wrong model was accepted")
			|| !check(md::validateMidiSysexStream(mmOsUpdate, md::MachineModel::Monomachine)
				== MidiSysexStreamValidation::FirmwareUpdate, "OS update was accepted")
			|| !check(md::validateMidiSysexStream(validDump, md::MachineModel::Machinedrum)
				== MidiSysexStreamValidation::Valid, "valid checksummed dump was rejected")
			|| !check(md::validateMidiSysexStream(mdRequest,
				md::MachineModel::Machinedrum)
				== MidiSysexStreamValidation::UnsupportedMessage,
				"non-import command was accepted"))
			return false;

		auto corruptDump = validDump;
		corruptDump[11] ^= 0x01;
		if(!check(md::validateMidiSysexStream(corruptDump,
				md::MachineModel::Machinedrum)
				== MidiSysexStreamValidation::ChecksumMismatch,
			"corrupt dump checksum was accepted"))
			return false;
		auto corruptDigiPro = validDigiProFooter;
		corruptDigiPro[10] ^= 0x01;
		if(!check(md::validateMidiSysexStream(corruptDigiPro,
				md::MachineModel::Monomachine)
				== MidiSysexStreamValidation::ChecksumMismatch,
			"corrupt DigiPRO checksum was accepted"))
			return false;
		auto invalidData = validDump;
		invalidData[10] = 0x80;
		if(!check(md::validateMidiSysexStream(invalidData,
				md::MachineModel::Machinedrum)
				== MidiSysexStreamValidation::InvalidDataByte,
			"non-7-bit SysEx data was accepted"))
			return false;

		auto concatenated = validDump;
		concatenated.insert(concatenated.end(), validDump.begin(), validDump.end());
		if(!check(md::validateMidiSysexStream(concatenated, md::MachineModel::Machinedrum)
				== MidiSysexStreamValidation::Valid, "concatenated data was rejected"))
			return false;
		concatenated.push_back(0x00);
		return check(md::validateMidiSysexStream(concatenated,
			md::MachineModel::Machinedrum) == MidiSysexStreamValidation::InvalidFraming,
			"trailing data was accepted");
	}

	bool testConcurrentServiceAndProgressReaders()
	{
		md::TurboMidiTransfer transfer(g_testClockHz);
		MidiSink sink;
		std::vector<uint8_t> payload(10000, 0x01);
		payload.front() = 0xf0;
		payload.back() = 0xf7;
		auto prepared = md::prepareMidiSysexTransfer(std::move(payload));
		if(!prepared || !transfer.start(*prepared, 0))
			return check(false, "concurrency transfer did not start");

		std::atomic<bool> invalidProgress{false};
		auto service = [&]
		{
			for(size_t i = 0; i < 100000
				&& transfer.progress().state != md::MidiSysexTransferState::Complete; ++i)
				transfer.service(1, true, sink);
		};
		auto observe = [&]
		{
			for(size_t i = 0; i < 100000; ++i)
			{
				const auto progress = transfer.progress();
				if(progress.sent > progress.total
					|| (progress.total != 0 && progress.total != 10000))
					invalidProgress.store(true, std::memory_order_relaxed);
				if(progress.state == md::MidiSysexTransferState::Complete)
					break;
			}
		};

		std::thread serviceA(service);
		std::thread serviceB(service); // intentional contract violation: gate must reject overlap
		std::thread observerA(observe);
		std::thread observerB(observe);
		serviceA.join();
		serviceB.join();
		observerA.join();
		observerB.join();

		const auto progress = transfer.progress();
		return check(!invalidProgress.load(std::memory_order_relaxed),
			"progress publisher returned a torn observation")
			&& check(progress.state == md::MidiSysexTransferState::Complete,
				"nonblocking serialization gate did not complete transfer")
			&& check(progress.sent == progress.total,
				"concurrent service duplicated or lost payload bytes");
	}

	bool testConcurrentStartHasOneOwner()
	{
		for(size_t iteration = 0; iteration < 100; ++iteration)
		{
			md::TurboMidiTransfer transfer(g_testClockHz);
			auto first = md::prepareMidiSysexTransfer({0xf0, 0x11, 0xf7});
			auto second = md::prepareMidiSysexTransfer({0xf0, 0x22, 0x23, 0xf7});
			if(!first || !second)
				return check(false, "concurrent-start fixtures were rejected");
			std::atomic<unsigned> winners{0};
			std::thread startA([&]
			{
				if(transfer.start(*first, 0))
					winners.fetch_add(1, std::memory_order_relaxed);
			});
			std::thread startB([&]
			{
				if(transfer.start(*second, 0))
					winners.fetch_add(1, std::memory_order_relaxed);
			});
			startA.join();
			startB.join();
			const auto progress = transfer.progress();
			if(!check(winners.load(std::memory_order_relaxed) == 1,
					"concurrent starts acquired more or fewer than one owner")
				|| !check(first->empty() != second->empty(),
					"concurrent start did not preserve the rejected payload")
				|| !check(progress.state == md::MidiSysexTransferState::Queued
					&& (progress.total == 3 || progress.total == 4),
					"concurrent start published an invalid winner"))
				return false;
		}
		return true;
	}
}

int main()
{
	if(!testTimeoutFallback() || !testDocumentedHandshake()
		|| !testCancellationAndRetirement()
		|| !testCompletedPayloadRetirement() || !testPausedServiceResumes()
		|| !testValidation()
		|| !testConcurrentServiceAndProgressReaders()
		|| !testConcurrentStartHasOneOwner())
		return 1;
	std::cout << "TurboMIDI unit tests passed\n";
	return 0;
}
