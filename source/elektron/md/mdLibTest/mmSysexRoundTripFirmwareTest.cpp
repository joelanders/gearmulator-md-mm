// Manual, firmware-backed round trip for "Record SysEx to File":
// machine A gets a renamed kit, SYSEX SEND -> ALL is recorded, the recording is
// imported into a fresh machine B, and B's own SEND ALL must reproduce it byte
// for byte.

#include "mdLib/mdsysexrecorder.h"
#include "mdLib/mdsysextransfer.h"
#include "sysexContentOracle.h"
#include "sysexPanelDriver.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace
{
	using namespace md::test;

	Sysex load(const char* _path)
	{
		std::ifstream stream(_path, std::ios::binary);
		return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	}

	void boot(md::Hardware& _hardware)
	{
		advanceFrames(_hardware, md::g_samplerate * 20);
		require(_hardware.isFirmwareMidiReady(), "MM boot incomplete");
	}

	// Run a prepared import, re-entering the receive screen whenever the transfer
	// pauses at a kind boundary, the same way a user follows the editor's prompts.
	void importFile(md::Hardware& _hardware, const Sysex& _bytes)
	{
		enterMmReceive(_hardware, splitSysex(_bytes).front()[6] == 0x5d);
		auto prepared = md::prepareMidiSysexTransfer(_bytes, md::MachineModel::Monomachine);
		require(prepared && _hardware.startMidiSysexTransfer(*prepared), "import start failed");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
		while(std::chrono::steady_clock::now() < deadline)
		{
			_hardware.advance(nextSysexTestBlockSize());
			const auto progress = _hardware.getMidiSysexTransferProgress();
			require(progress.state != md::MidiSysexTransferState::Failed, "import failed");
			if(progress.state == md::MidiSysexTransferState::Complete)
			{
				panelTap(_hardware, md::PanelControl::Exit);
				advanceFrames(_hardware, md::g_samplerate * 20);
				exitMenus(_hardware);
				return;
			}
			if(progress.state != md::MidiSysexTransferState::WaitingForReceiveMode)
				continue;
			std::printf("MM ROUNDTRIP import boundary kind=%u step=%zu sent=%zu\n",
				unsigned(progress.receiveKind), progress.receiveStep, progress.sent);
			exitMenus(_hardware);
			advanceFrames(_hardware, md::g_samplerate * 3);
			enterMmReceive(_hardware, progress.receiveKind == md::MidiSysexMessageKind::DigiPro);
			require(_hardware.resumeMidiSysexReceiveMode(progress.transferId, progress.receiveStep),
				"resume rejected");
		}
		require(false, "import deadline expired");
	}

	// Drive SYSEX SEND -> ALL and feed the host-visible output through the same
	// recorder the editor uses.
	Sysex recordSendAll(md::Hardware& _hardware, const std::string& _lcdImage)
	{
		// The firmware remembers menu cursors, and an import leaves the FILE menu
		// on SYSEX RECV. Home every cursor first, as enterMmReceive does.
		exitMenus(_hardware);
		panelChord(_hardware, md::PanelControl::Kit);
		panelTap(_hardware, md::PanelControl::Enter);
		for(unsigned i = 0; i < 4; ++i) panelTap(_hardware, md::PanelControl::Left);
		for(unsigned i = 0; i < 8; ++i) panelTap(_hardware, md::PanelControl::Up);
		panelTap(_hardware, md::PanelControl::Down);
		panelTap(_hardware, md::PanelControl::Down);
		panelTap(_hardware, md::PanelControl::Right);
		for(unsigned i = 0; i < 8; ++i) panelTap(_hardware, md::PanelControl::Up);
		panelTap(_hardware, md::PanelControl::Enter);
		if(!_lcdImage.empty())
			panelImage(_hardware, _lcdImage);

		std::vector<synthLib::SMidiEvent> events;
		_hardware.readMidiOut(events);
		md::MidiSysexRecorder recorder(md::MachineModel::Monomachine);
		Sysex storage;
		storage.reserve(md::g_midiSysexTransferMaxBytes);
		require(recorder.start(storage, _hardware.midiTxOverflowCount()), "recorder start failed");

		uint32_t idleFrames = 0;
		const auto drain = [&](const uint32_t _frames)
		{
			const auto before = recorder.status().messages;
			_hardware.advance(_frames);
			events.clear();
			_hardware.readMidiOut(events);
			recorder.capture(events, 0, _hardware.midiTxOverflowCount());
			idleFrames = recorder.status().messages != before ? 0 : idleFrames + _frames;
		};
		const auto enter = panelPacket(_hardware.getModel(), md::PanelControl::Enter);
		require(enter.has_value(), "missing MM enter control");
		require(_hardware.trySendPanelEvent(enter->row, enter->mask), "send-all press rejected");
		for(uint32_t frames = 0; frames < 2048; frames += 64)
			drain(64);
		require(_hardware.trySendPanelEvent(enter->row, 0), "send-all release rejected");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
		while(std::chrono::steady_clock::now() < deadline)
		{
			drain(nextSysexTestBlockSize());
			if(recorder.status().messages >= 128 && idleFrames >= md::g_samplerate * 5)
				break;
		}

		Sysex recorded;
		md::MidiSysexRecordingStatus status;
		require(recorder.stop(recorded, status), "recorder stop failed");
		std::printf("MM ROUNDTRIP recorded messages=%zu bytes=%zu skipped=%zu truncated=%u\n",
			status.messages, status.bytes, status.skipped, status.truncated);
		require(!status.truncated, "recording lost bytes");
		require(md::parseMidiSysexFile(recorded, md::MachineModel::Monomachine)
			== md::MidiSysexStreamValidation::Valid, "recording is not an importable file");
		exitMenus(_hardware);
		return recorded;
	}

	// Literal-RLE kit rename, independent of the firmware's own compression.
	Sysex renamedKit(md::Hardware& _hardware, const uint8_t _slot, const std::string& _name)
	{
		const auto original = requestDump(_hardware, {0xf0, 0, 0x20, 0x3c, 3, 0, 0x52, 2, 1, _slot});
		require(original.size() > 15, "kit dump request failed");
		const auto packed = unpackElektron(original.begin() + 10, original.end() - 5);
		Sysex raw;
		for(size_t i = 0; i < packed.size(); ++i)
		{
			if(!(packed[i] & 128))
			{
				raw.push_back(packed[i]);
				continue;
			}
			const auto count = packed[i] & 127;
			require(count && i + 1 < packed.size(), "invalid MM run length");
			raw.insert(raw.end(), count, packed[++i]);
		}
		require(raw.size() > 11 && _name.size() <= 11, "kit name fixture invalid");
		std::fill_n(raw.begin(), 11, 0);
		std::copy(_name.begin(), _name.end(), raw.begin());
		Sysex literal;
		for(const auto b : raw)
		{
			if(b & 128)
				literal.push_back(0x81);
			literal.push_back(b);
		}
		Sysex message(original.begin(), original.begin() + 10);
		for(size_t i = 0; i < literal.size();)
		{
			const size_t n = std::min<size_t>(7, literal.size() - i);
			uint8_t high = 0;
			for(size_t j = 0; j < n; ++j)
				high |= ((literal[i + j] >> 7) << (6 - j));
			message.push_back(high);
			for(size_t j = 0; j < n; ++j)
				message.push_back(literal[i++] & 127);
		}
		unsigned sum = 0;
		for(size_t i = 9; i < message.size(); ++i)
			sum += message[i];
		const auto length = message.size() - 5;
		message.insert(message.end(), {uint8_t((sum >> 7) & 127), uint8_t(sum & 127),
			uint8_t((length >> 7) & 127), uint8_t(length & 127), 0xf7});
		return message;
	}

	std::string kitName(md::Hardware& _hardware, const uint8_t _slot)
	{
		const auto dump = requestDump(_hardware, {0xf0, 0, 0x20, 0x3c, 3, 0, 0x52, 2, 1, _slot});
		require(dump.size() > 15, "kit dump request failed");
		const auto packed = unpackElektron(dump.begin() + 10, dump.end() - 5);
		std::string name;
		for(size_t i = 0; i < packed.size() && name.size() < 11; ++i)
		{
			if(packed[i] & 128)
			{
				name.append(packed[i] & 127, char(packed[i + 1]));
				++i;
			}
			else
				name.push_back(char(packed[i]));
		}
		name.resize(std::min<size_t>(name.size(), 11));
		while(!name.empty() && name.back() == '\0')
			name.pop_back();
		return name;
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
	if(argc != 3 && argc != 4)
	{
		std::puts("usage: mmSysexRoundTripFirmwareTest <MM-ROM> <1MiB-patch-ram> [output-prefix]");
		return 2;
	}
	try
	{
		const auto rom = load(argv[1]);
		const auto patchRam = load(argv[2]);
		require(patchRam.size() == 0x100000, "MM patch RAM must be exactly 1 MiB");
		const std::string prefix = argc == 4 ? argv[3] : "";
		constexpr uint8_t slot = 5;
		const std::string marker = "ROUND TRIP";

		auto source = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Monomachine, patchRam);
		require(source->isValid(), "firmware did not construct a valid machine");
		boot(*source);
		const auto factoryName = kitName(*source, slot);
		require(factoryName != marker, "factory kit already carries the marker name");
		importFile(*source, renamedKit(*source, slot, marker));
		require(kitName(*source, slot) == marker, "kit rename did not take on the source machine");
		const auto first = recordSendAll(*source, prefix.empty() ? "" : prefix + "-send-a.pgm");
		source.reset();

		auto target = std::make_unique<md::Hardware>(rom, argv[1], md::MachineModel::Monomachine, patchRam);
		boot(*target);
		require(kitName(*target, slot) == factoryName, "fresh machine does not start from factory data");
		importFile(*target, first);
		require(kitName(*target, slot) == marker, "imported recording did not restore the renamed kit");
		const auto second = recordSendAll(*target, prefix.empty() ? "" : prefix + "-send-b.pgm");

		if(argc == 4)
		{
			writeFile(std::string(argv[3]) + "-first.syx", first);
			writeFile(std::string(argv[3]) + "-second.syx", second);
		}
		const auto a = splitSysex(first);
		const auto b = splitSysex(second);
		size_t differing = 0;
		for(size_t i = 0; i < std::min(a.size(), b.size()); ++i)
			if(a[i] != b[i])
			{
				if(differing++ < 8)
					std::printf("MM ROUNDTRIP differs message=%zu command=%02x slot=%u sizes=%zu/%zu\n",
						i, a[i][6], a[i][9], a[i].size(), b[i].size());
			}
		std::printf("MM ROUNDTRIP first=%zu/%zu bytes second=%zu/%zu bytes differing=%zu\n",
			a.size(), first.size(), b.size(), second.size(), differing);
		require(a.size() == 288, "SEND ALL did not record every user-data dump");
		require(first == second, "re-exported recording differs from the original");
		std::puts("MM ROUNDTRIP passed");
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "MM ROUNDTRIP FAIL: %s\n", _error.what());
		return 1;
	}
}
