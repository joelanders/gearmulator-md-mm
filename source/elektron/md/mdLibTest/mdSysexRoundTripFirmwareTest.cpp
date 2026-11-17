// Manual, firmware-backed round trip for "Record SysEx to File" on the
// Machinedrum: machine A gets a renamed kit, every kit, pattern, song and
// global is requested and recorded, the recording is imported into a fresh
// machine B, and B's own dumps must reproduce it byte for byte.
//
// MD answers dump requests on any screen, so this drives export by request
// rather than through the panel's SYSEX SEND page; the recorder sees the same
// machine-originated messages either way.

#include "mdLib/mdhardware.h"
#include "mdLib/mdsysexrecorder.h"
#include "sysexContentOracle.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	using md::test::Sysex;
	using md::test::splitSysex;

	constexpr uint8_t g_product = 2;

	struct DumpKind
	{
		uint8_t command;
		uint8_t count;
		const char* name;
	};

	constexpr DumpKind g_kinds[] = {
		{0x50, 8, "globals"}, {0x52, 64, "kits"}, {0x67, 128, "patterns"}, {0x69, 32, "songs"}};

	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	Sysex load(const char* _path)
	{
		std::ifstream stream(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}

	void advance(md::Hardware& _hardware, uint32_t _frames)
	{
		std::vector<synthLib::SMidiEvent> discarded;
		while(_frames)
		{
			const auto n = std::min<uint32_t>(_frames, 64);
			_hardware.advance(n);
			discarded.clear();
			_hardware.readMidiOut(discarded);
			_frames -= n;
		}
	}

	void boot(md::Hardware& _hardware)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while(!_hardware.isFirmwareMidiReady())
		{
			_hardware.advance(64);
			require(std::chrono::steady_clock::now() < deadline, "MD firmware boot timed out");
		}
		advance(_hardware, md::g_samplerate * 10);
	}

	// Request one dump and feed every machine message through the recorder.
	bool requestInto(md::Hardware& _hardware, md::MidiSysexRecorder& _recorder,
		const uint8_t _command, const uint8_t _slot)
	{
		synthLib::SMidiEvent request(synthLib::MidiEventSource::Host);
		request.sysex = {0xf0, 0, 0x20, 0x3c, g_product, 0, uint8_t(_command + 1), _slot, 0xf7};
		require(_hardware.sendMidi(request), "dump request was rejected");
		std::vector<synthLib::SMidiEvent> events;
		for(size_t i = 0; i < md::g_samplerate * 5 / 64; ++i)
		{
			_hardware.advance(64);
			events.clear();
			_hardware.readMidiOut(events);
			const auto before = _recorder.status().messages;
			_recorder.capture(events, 0, _hardware.midiTxOverflowCount());
			if(_recorder.status().messages == before)
				continue;
			for(const auto& event : events)
				if(event.sysex.size() > 10 && event.sysex[6] == _command && event.sysex[9] == _slot)
					return true;
		}
		return false;
	}

	Sysex recordEverything(md::Hardware& _hardware)
	{
		md::MidiSysexRecorder recorder(md::MachineModel::Machinedrum);
		Sysex storage;
		storage.reserve(md::g_midiSysexTransferMaxBytes);
		require(recorder.start(storage, _hardware.midiTxOverflowCount()), "recorder start failed");
		for(const auto& kind : g_kinds)
			for(uint8_t slot = 0; slot < kind.count; ++slot)
				if(!requestInto(_hardware, recorder, kind.command, slot))
				{
					std::fprintf(stderr, "MD ROUNDTRIP no %s dump for slot %u\n", kind.name, slot);
					require(false, "machine did not answer a dump request");
				}
		Sysex recorded;
		md::MidiSysexRecordingStatus status;
		require(recorder.stop(recorded, status), "recorder stop failed");
		std::printf("MD ROUNDTRIP recorded messages=%zu bytes=%zu skipped=%zu truncated=%u\n",
			status.messages, status.bytes, status.skipped, status.truncated);
		require(!status.truncated, "recording lost bytes");
		const auto validation = md::parseMidiSysexFile(recorded, md::MachineModel::Machinedrum);
		if(validation != md::MidiSysexStreamValidation::Valid)
			std::fprintf(stderr, "MD ROUNDTRIP validation: %s\n", md::midiSysexValidationMessage(validation));
		require(validation == md::MidiSysexStreamValidation::Valid, "recording is not an importable file");
		return recorded;
	}

	void importFile(md::Hardware& _hardware, const Sysex& _bytes)
	{
		auto prepared = md::prepareMidiSysexTransfer(_bytes, md::MachineModel::Machinedrum);
		require(prepared && _hardware.startMidiSysexTransfer(*prepared), "import start failed");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
		std::vector<synthLib::SMidiEvent> discarded;
		while(_hardware.getMidiSysexTransferProgress().state != md::MidiSysexTransferState::Complete)
		{
			_hardware.advance(64);
			discarded.clear();
			_hardware.readMidiOut(discarded);
			require(_hardware.getMidiSysexTransferProgress().state != md::MidiSysexTransferState::Failed,
				"import failed");
			require(std::chrono::steady_clock::now() < deadline, "import deadline expired");
		}
		// UART-drained is a transport result; let the firmware commit the last message.
		advance(_hardware, md::g_samplerate * 5);
	}

	Sysex kitDump(md::Hardware& _hardware, const uint8_t _slot)
	{
		const auto dump = md::test::requestDump(_hardware, {0xf0, 0, 0x20, 0x3c, g_product, 0, 0x52, 0, 0, _slot});
		require(dump.size() > 30, "kit dump request failed");
		return dump;
	}

	// The MD kit name is NUL-terminated 7-bit text at offset 0x0a. Observed on
	// 1.63 factory kits: 11 bytes, followed by non-text kit data at 0x15.
	std::string kitName(const Sysex& _dump)
	{
		std::string name(_dump.begin() + 10, _dump.begin() + 21);
		name.resize(std::min(name.size(), name.find('\0')));
		while(!name.empty() && (name.back() == '\0' || name.back() == ' '))
			name.pop_back();
		return name;
	}

	Sysex renamedKit(Sysex _dump, const uint8_t _slot, const std::string& _name)
	{
		_dump[9] = _slot;
		std::fill(_dump.begin() + 10, _dump.begin() + 21, uint8_t{0});
		std::copy(_name.begin(), _name.end(), _dump.begin() + 10);
		unsigned sum = 0;
		for(size_t i = 9; i < _dump.size() - 5; ++i)
			sum += _dump[i];
		_dump[_dump.size() - 5] = uint8_t((sum >> 7) & 0x7f);
		_dump[_dump.size() - 4] = uint8_t(sum & 0x7f);
		return _dump;
	}

	void writeFile(const std::string& _path, const Sysex& _bytes)
	{
		std::ofstream out(_path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(_bytes.data()), std::streamsize(_bytes.size()));
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	if(argc != 2 && argc != 3)
	{
		std::puts("usage: mdSysexRoundTripFirmwareTest <MD-ROM> [output-prefix]");
		return 2;
	}
	try
	{
		const auto rom = load(argv[1]);
		const std::string prefix = argc == 3 ? argv[2] : "";
		constexpr uint8_t slot = 5;
		const std::string marker = "ROUND TRIP";

		auto source = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum, Sysex{});
		require(source->isValid(), "firmware did not construct a valid machine");
		boot(*source);
		const auto factory = kitDump(*source, slot);
		const auto factoryName = kitName(factory);
		std::printf("MD ROUNDTRIP factory kit %u name='%s'\n", slot, factoryName.c_str());
		for(size_t i = 10; i < 10 + factoryName.size(); ++i)
			require(factory[i] >= 0x20 && factory[i] < 0x7f,
				"kit name field is not plain ASCII; the offset assumption is wrong");
		require(factoryName != marker, "factory kit already carries the marker name");
		const auto renamed = renamedKit(kitDump(*source, 0), slot, marker);
		require(md::parseMidiSysexFile(renamed, md::MachineModel::Machinedrum)
			== md::MidiSysexStreamValidation::Valid, "renamed kit fixture is invalid");
		importFile(*source, renamed);
		require(kitName(kitDump(*source, slot)) == marker, "kit rename did not take on the source machine");
		const auto first = recordEverything(*source);
		source.reset();

		auto target = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Machinedrum, Sysex{});
		boot(*target);
		require(kitName(kitDump(*target, slot)) == factoryName, "fresh machine does not start from factory data");
		importFile(*target, first);
		require(kitName(kitDump(*target, slot)) == marker, "imported recording did not restore the renamed kit");
		const auto second = recordEverything(*target);

		if(!prefix.empty())
		{
			writeFile(prefix + "-first.syx", first);
			writeFile(prefix + "-second.syx", second);
		}
		const auto a = splitSysex(first);
		const auto b = splitSysex(second);
		size_t differing = 0;
		for(size_t i = 0; i < std::min(a.size(), b.size()); ++i)
			if(a[i] != b[i] && differing++ < 8)
				std::printf("MD ROUNDTRIP differs message=%zu command=%02x slot=%u sizes=%zu/%zu\n",
					i, a[i][6], a[i][9], a[i].size(), b[i].size());
		std::printf("MD ROUNDTRIP first=%zu/%zu bytes second=%zu/%zu bytes differing=%zu\n",
			a.size(), first.size(), b.size(), second.size(), differing);
		require(a.size() == 8 + 64 + 128 + 32, "recording is missing dumps");
		require(first == second, "re-exported recording differs from the original");
		std::puts("MD ROUNDTRIP passed");
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "MD ROUNDTRIP FAIL: %s\n", _error.what());
		return 1;
	}
}
