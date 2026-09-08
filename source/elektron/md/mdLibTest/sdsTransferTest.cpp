#include "mdLib/mdturbomidi.h"
#include "sdsTestData.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <limits>
#include <vector>

namespace
{
	using Result = md::MidiSysexStreamValidation;
	using State = md::MidiSysexTransferState;
	bool check(bool success, const char* message)
	{
		if(!success) std::fprintf(stderr, "%s\n", message);
		return success;
	}
	std::vector<uint8_t> dump(uint8_t product = 2)
	{
		return {0xf0, 0, 0x20, 0x3c, product, 0, 0x52, 4, 1, 1, 0, 1, 0, 5, 0xf7};
	}
	bool validation()
	{
		const auto sample = md::test::sdsSample();
		const auto valid = [](const auto& bytes) { return md::validateMidiSysexStream(bytes, md::MachineModel::Machinedrum); };
		if(!check(valid(sample) == Result::Valid, "generated sample rejected")) return false;
		auto legacyDump = dump();
		legacyDump[5] = 3;
		legacyDump[7] = 0x40;
		if(!check(valid(legacyDump) == Result::Valid, "existing header/version tolerance regressed")) return false;
		for(uint8_t bits : {8, 12, 14, 16, 21, 28})
			if(!check(valid(md::test::sdsSample(137, bits)) == Result::Valid, "SDS resolution rejected")) return false;
		auto bank = sample;
		const auto second = md::test::sdsSample(80, 12, 3, 1);
		bank.insert(bank.end(), second.begin(), second.end());
		if(!check(valid(bank) == Result::Valid, "sample bank rejected")
			|| !check(md::validateMidiSysexStream(sample, md::MachineModel::Monomachine) == Result::WrongModel, "MM accepted SDS")) return false;
		auto broken = sample;
		broken.pop_back();
		if(!check(valid(broken) == Result::InvalidFraming, "truncated message accepted")) return false;
		broken = sample; broken.resize(34 + 127);
		if(!check(valid(broken) == Result::InvalidSampleSequence, "truncated sample accepted")) return false;
		broken = sample; broken[34 + 5] ^= 1;
		if(!check(valid(broken) == Result::ChecksumMismatch, "bad XOR accepted")) return false;
		broken = sample; broken[34 + 4] = 1;
		if(!check(valid(broken) == Result::InvalidSampleSequence, "bad sequence accepted")) return false;
		broken = sample; broken[6] = 29;
		if(!check(valid(broken) == Result::InvalidSampleHeader, "invalid resolution accepted")) return false;
		broken = sample; broken[7] = broken[8] = broken[9] = 0;
		if(!check(valid(broken) == Result::InvalidSampleHeader, "zero sample period accepted")) return false;
		broken = sample; broken[34 + 5] = 0x80;
		if(!check(valid(broken) == Result::InvalidDataByte, "non-7-bit sample accepted")) return false;
		broken = sample; broken.insert(broken.end(), sample.begin() + 34, sample.begin() + 34 + 127);
		if(!check(valid(broken) == Result::InvalidSampleSequence, "extra packet accepted")) return false;
		const std::vector<std::pair<std::vector<uint8_t>, Result>> commands{
			{{0xf0, 0x7e, 0, 0x7f, 0, 0xf7}, Result::TransportMessage},
			{{0xf0, 0x7e, 0, 5, 3, 0xf7}, Result::UnsupportedSampleExtension},
			{{0xf0, 0x7e, 0, 3, 0, 0, 0xf7}, Result::UnsupportedMessage},
			{{0xf0, 0, 0x20, 0x3c, 2, 0, 0x7e, 0xf7}, Result::FirmwareUpdate},
			{{0xf0, 0, 0x20, 0x3c, 0, 0, 0x10, 0xf7}, Result::TransportMessage},
			{{0xf0, 0, 0x20, 0x3c, 2, 0, 0x40, 0xf7}, Result::UnknownElektronCommand},
			{{0xf0, 0x41, 0, 0xf7}, Result::OtherManufacturer}
		};
		for(const auto& [bytes, expected] : commands)
		{
			Result result{};
			if(!check(!md::prepareMidiSysexTransfer(bytes, md::MachineModel::Machinedrum, &result)
				&& result == expected, "preparation bypassed classification")) return false;
		}
		return true;
	}

	class Sink final : public md::MidiByteSink
	{
	public:
		bool tryWriteMidiByte(uint8_t byte) override
		{
			if(bytes.size() >= limit) return false;
			bytes.push_back(byte);
			return true;
		}
		size_t queuedMidiByteCount() const override { return queued; }
		std::vector<uint8_t> bytes;
		size_t limit = std::numeric_limits<size_t>::max();
		size_t queued = 0;
	};

	void reply(md::TurboMidiTransfer& transfer, uint8_t command, uint8_t packet, uint8_t device = 0)
	{
		for(auto b : {uint8_t{0xf0}, uint8_t{0x7e}, device, command, packet, uint8_t{0xf7}})
			transfer.observeTransmitByte(b);
	}

	enum class Scenario { Normal, Nak, LostAck, LostAckNakNext, LostAckNakNextWrap, Wait, WaitTimeout, Cancel, Silence, WrongReply };
	bool transferTest(Scenario scenario)
	{
		md::TurboMidiTransfer transfer(1000);
		Sink sink;
		auto bytes = md::test::sdsSample(5201, 16, 9);
		const auto second = md::test::sdsSample(5, 12, 0, 1);
		bytes.insert(bytes.end(), second.begin(), second.end());
		const auto kit = dump();
		bytes.insert(bytes.begin(), kit.begin(), kit.end());
		bytes.insert(bytes.end(), kit.begin(), kit.end());
		auto prepared = md::prepareMidiSysexTransfer(bytes);
		if(!prepared || !transfer.start(*prepared, 0)) return false;
		size_t read = 0, packets = 0;
		bool disturbed = false;
		const uint8_t disturbancePacket = scenario == Scenario::LostAckNakNextWrap ? 127 : 0;
		std::vector<uint8_t> incoming;
		uint32_t waitUntil = 0;
		uint8_t waitPacket = 0;
		for(uint32_t tick = 0; tick < 45000 && transfer.ownsMidiWire(); ++tick)
		{
			transfer.service(1, true, sink);
			const auto progress = transfer.progress();
			if(progress.state == State::WaitingForReceiveMode)
				transfer.resumeReceiveMode(progress.transferId, progress.receiveStep);
			if(waitUntil && tick == waitUntil) reply(transfer, 0x7f, waitPacket);
			while(read < sink.bytes.size())
			{
				const auto b = sink.bytes[read++];
				if(b >= 0xf8) continue;
				if(b == 0xf0) incoming.clear();
				if(incoming.empty() && b != 0xf0) continue;
				incoming.push_back(b);
				if(b != 0xf7) continue;
				auto complete = std::move(incoming);
				incoming.clear();
				const auto& incoming = complete;
				if(incoming.size() < 5 || incoming[1] != 0x7e) continue;
				if(incoming[3] != 1 && incoming[3] != 2) continue;
				if(!check(incoming[2] == 0, "SDS was not retargeted to MD device 0")) return false;
				const uint8_t packet = incoming[3] == 1 ? 0 : incoming[4];
				if(incoming[3] == 2)
				{
					++packets;
					uint8_t sum = 0;
					for(size_t i = 1; i < incoming.size() - 1; ++i) sum ^= incoming[i];
					if(!check(sum == 0, "retargeted packet checksum is wrong"))
					{ std::fprintf(stderr, "scenario=%u length=%zu packet=%u sum=%u\n", unsigned(scenario), incoming.size(), packet, sum); return false; }
				}
				if(scenario == Scenario::Silence) continue;
				if((scenario == Scenario::LostAckNakNext || scenario == Scenario::LostAckNakNextWrap)
					&& disturbed && incoming[3] == 2 && packets == size_t(disturbancePacket) + 2)
				{
					reply(transfer, 0x7e, (packet + 1) & 0x7f);
					continue;
				}
				if(scenario == Scenario::WrongReply)
				{
					reply(transfer, 0x7f, packet, 2);
					reply(transfer, 0x7f, (packet + 1) & 0x7f);
					continue;
				}
				if(incoming[3] == 2 && !disturbed && scenario != Scenario::Normal && packet == disturbancePacket)
				{
					disturbed = true;
					if(scenario == Scenario::Nak) reply(transfer, 0x7e, packet);
					if(scenario == Scenario::Cancel) reply(transfer, 0x7d, packet);
					if(scenario == Scenario::Wait || scenario == Scenario::WaitTimeout)
					{
						reply(transfer, 0x7c, packet);
						waitPacket = packet;
						waitUntil = scenario == Scenario::Wait ? tick + 3000 : 0;
					}
					continue;
				}
				reply(transfer, 0x7f, packet);
			}
			if(!check(transfer.progress().sent <= bytes.size(), "retries inflated progress")) return false;
		}
		const auto progress = transfer.progress();
		const bool failure = scenario == Scenario::Silence || scenario == Scenario::WrongReply
			|| scenario == Scenario::WaitTimeout || scenario == Scenario::Cancel;
		if(failure)
			return check(progress.state == State::Failed && progress.error != md::MidiSysexTransferError::None,
				"unresponsive/cancelled SDS incorrectly completed");
		const auto retries = scenario == Scenario::Nak || scenario == Scenario::LostAck
			|| scenario == Scenario::LostAckNakNext || scenario == Scenario::LostAckNakNextWrap ? 1u : 0u;
		return check(progress.state == State::Complete && progress.sent == bytes.size()
			&& progress.acknowledgedSamples == 2 && progress.retries == retries && packets == 132 + retries,
			"SDS bank transfer/retry/wrap failed");
	}

	bool cancellationAndDrain()
	{
		md::TurboMidiTransfer transfer(1000);
		Sink sink;
		auto prepared = md::prepareMidiSysexTransfer(md::test::sdsSample());
		if(!prepared || !transfer.start(*prepared, 0)) return false;
		transfer.service(1, true, sink);
		transfer.service(1000, true, sink);
		sink.queued = 1;
		transfer.service(1001, true, sink);
		// Header is buffered but has not drained. Even an early ACK cannot let
		// another message overtake it or start a premature response timeout.
		reply(transfer, 0x7f, 0);
		const auto count = sink.bytes.size();
		transfer.service(5000, true, sink);
		if(!check(sink.bytes.size() == count && transfer.progress().retries == 0,
			"SDS timeout began before UART drain")) return false;
		std::vector<uint8_t> retired;
		if(!transfer.cancel(retired)) return false;
		transfer.service(1000, true, sink);
		if(!check(transfer.ownsMidiWire(), "cancellation released a nonempty UART")) return false;
		sink.queued = 0;
		transfer.service(1, true, sink);
		const std::vector<uint8_t> suffix{0xf7, 0xf0, 0x7e, 0, 0x7d, 0, 0xf7};
		return check(transfer.progress().state == State::Cancelled
			&& std::equal(suffix.rbegin(), suffix.rend(), sink.bytes.rbegin()), "SDS cancel was not sent");
	}

	bool mixedMmReceiveModes()
	{
		const auto kit = dump(3);
		const std::vector<uint8_t> wave{0xf0, 0, 0x20, 0x3c, 3, 0, 0x5d, 1, 1,
			0x21, 2, 0, 2, 0, 6, 0xf7};
		auto bytes = kit;
		bytes.insert(bytes.end(), wave.begin(), wave.end());
		bytes.insert(bytes.end(), kit.begin(), kit.end());
		auto prepared = md::prepareMidiSysexTransfer(bytes, md::MachineModel::Monomachine);
		md::TurboMidiTransfer transfer(1000);
		Sink sink;
		if(!prepared || !transfer.start(*prepared, 0)) return false;
		transfer.service(1000, true, sink);
		transfer.service(1001, true, sink);
		auto progress = transfer.progress();
		if(!check(progress.state == State::WaitingForReceiveMode && progress.sent == kit.size()
			&& progress.receiveKind == md::MidiSysexMessageKind::DigiPro,
			"mixed MM file did not pause before wave data")) return false;
		if(!check(!transfer.resumeReceiveMode(progress.transferId + 1, progress.receiveStep)
			&& !transfer.resumeReceiveMode(progress.transferId, progress.receiveStep + 1),
			"stale receive-screen action resumed another stage")) return false;
		transfer.service(60000, true, sink);
		if(!check(transfer.progress().sent == kit.size(), "UI pause sent more data")) return false;
		if(!transfer.resumeReceiveMode(progress.transferId, progress.receiveStep)) return false;
		transfer.service(1000, true, sink);
		progress = transfer.progress();
		if(!check(progress.state == State::WaitingForReceiveMode && progress.sent == kit.size() + wave.size()
			&& progress.receiveKind == md::MidiSysexMessageKind::UserDump,
			"mixed MM file did not pause before returning to general receive")) return false;
		if(!transfer.resumeReceiveMode(progress.transferId, progress.receiveStep)) return false;
		transfer.service(1000, true, sink);
		return check(transfer.progress().state == State::Complete && transfer.progress().sent == bytes.size(),
			"mixed MM file did not complete after screen changes");
	}

	bool responseOverflow()
	{
		md::TurboMidiTransfer transfer(1000);
		Sink sink;
		auto prepared = md::prepareMidiSysexTransfer(md::test::sdsSample());
		if(!prepared || !transfer.start(*prepared, 0)) return false;
		transfer.service(1000, true, sink);
		transfer.service(1001, true, sink);
		for(size_t i = 0; i < 9; ++i) reply(transfer, 0x7c, 0);
		transfer.service(1000, true, sink);
		return check(transfer.progress().state == State::Failed
			&& transfer.progress().error == md::MidiSysexTransferError::ResponseOverflow,
			"overflow silently lost a sample handshake");
	}

	bool mutatedInputs()
	{
		uint32_t random = 0x12345678;
		const auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
		const auto original = md::test::sdsSample(137);
		for(size_t iteration = 0; iteration < 2000; ++iteration)
		{
			auto bytes = original;
			for(size_t n = 0; n < 3; ++n) bytes[next() % bytes.size()] = uint8_t(next());
			if(iteration & 1) bytes.resize(next() % bytes.size());
			const auto expected = md::validateMidiSysexStream(bytes, md::MachineModel::Machinedrum);
			Result result{};
			const auto prepared = md::prepareMidiSysexTransfer(bytes, md::MachineModel::Machinedrum, &result);
			if(!check(expected == result && prepared.has_value() == (result == Result::Valid),
				"preparation and validation disagree on mutated input")) return false;
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	if(argc > 1)
	{
		const std::string mode = argv[1];
		if(argc < 3 || (mode != "--validate-md" && mode != "--validate-mm")) return 2;
		const auto model = mode == "--validate-mm" ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
		bool success = true;
		for(int i = 2; i < argc; ++i)
		{
			std::ifstream file(argv[i], std::ios::binary | std::ios::ate);
			const auto size = file.tellg();
			if(!file || size < 0 || size > md::g_midiSysexTransferMaxBytes)
			{
				std::fprintf(stderr, "%s: unreadable or exceeds input limit\n", argv[i]);
				success = false;
				continue;
			}
			file.seekg(0);
			std::vector<uint8_t> bytes(static_cast<size_t>(size));
			file.read(reinterpret_cast<char*>(bytes.data()), size);
			if(!file) { success = false; continue; }
			md::MidiSysexStreamValidation result{};
			const auto prepared = md::prepareMidiSysexTransfer(std::move(bytes), model, &result);
			std::printf("%s: %s\n", argv[i], md::midiSysexValidationMessage(result));
			success &= prepared.has_value();
		}
		return success ? 0 : 1;
	}
	if(!validation() || !mutatedInputs() || !cancellationAndDrain() || !mixedMmReceiveModes() || !responseOverflow()) return 1;
	for(auto scenario : {Scenario::Normal, Scenario::Nak, Scenario::LostAck, Scenario::LostAckNakNext, Scenario::LostAckNakNextWrap, Scenario::Wait,
		Scenario::WaitTimeout, Scenario::Cancel, Scenario::Silence, Scenario::WrongReply})
		if(!transferTest(scenario)) return 1;
	std::puts("SDS parser and transport tests passed");
	return 0;
}
