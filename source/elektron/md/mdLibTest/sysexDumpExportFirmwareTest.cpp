// Manual, firmware-backed check of the editor's request-driven SysEx export
// shortcuts through md::Device, the path the right-click menu uses.
//
//   mdSysexDumpExportFirmwareTest md <MD-ROM>
//   mdSysexDumpExportFirmwareTest mm <MM-ROM> <1MiB-patch-ram>

#include "mdLib/mddevice.h"
#include "mdLib/mdpanel.h"
#include "mdLib/mdsysexfile.h"
#include "sysexContentOracle.h"

#include <array>
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

	class Runner
	{
	public:
		explicit Runner(md::Device& _device) : m_device(_device)
		{
			for(size_t i = 0; i < m_out.size(); ++i)
				m_outputs[i] = m_out[i].data();
			m_inputs = {m_in.data(), m_in.data(), nullptr, nullptr};
		}

		void run(uint32_t _frames, const std::vector<synthLib::SMidiEvent>& _midiIn = {})
		{
			auto pending = _midiIn;
			while(_frames)
			{
				const auto n = std::min<uint32_t>(_frames, g_block);
				m_device.process(m_inputs, m_outputs, n, pending, m_midiOut);
				pending.clear();
				_frames -= n;
			}
		}

		void seconds(const double _seconds) { run(static_cast<uint32_t>(md::g_samplerate * _seconds)); }

	private:
		static constexpr uint32_t g_block = 64;
		md::Device& m_device;
		std::array<float, g_block> m_in{};
		std::array<std::array<float, g_block>, 12> m_out{};
		synthLib::TAudioInputs m_inputs{};
		synthLib::TAudioOutputs m_outputs{};
		std::vector<synthLib::SMidiEvent> m_midiOut;
	};

	Sysex runExport(md::Device& _device, Runner& _runner, const md::SysexDumpScope _scope,
		const bool _saveKitFirst, md::SysexDumpExportProgress& _progress)
	{
		Sysex storage;
		storage.reserve(md::g_midiSysexTransferMaxBytes);
		require(_device.startSysexDumpExport(_scope, _saveKitFirst, storage), "export did not start");
		require(!_device.startSysexDumpExport(_scope, _saveKitFirst, storage), "second export started");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(600);
		while(_device.sysexDumpExportProgress().state == md::SysexDumpExportState::Running)
		{
			_runner.run(512);
			require(std::chrono::steady_clock::now() < deadline, "export deadline expired");
		}
		_progress = _device.sysexDumpExportProgress();
		Sysex bytes;
		md::MidiSysexRecordingStatus status;
		require(_device.stopSysexRecording(bytes, status), "recording was not active");
		require(_device.sysexDumpExportProgress().state == md::SysexDumpExportState::Idle, "export not reset");
		std::printf("EXPORT scope=%u saveFirst=%u state=%u done=%zu/%zu slot=%u messages=%zu bytes=%zu truncated=%u\n",
			unsigned(_scope), _saveKitFirst, unsigned(_progress.state), _progress.done, _progress.total,
			_progress.slot, status.messages, bytes.size(), status.truncated);
		require(_progress.state == md::SysexDumpExportState::Complete, "export did not complete");
		require(!status.truncated && status.messages == _progress.total, "recording does not match the plan");
		require(md::parseMidiSysexFile(bytes, _device.getModel()) == md::MidiSysexStreamValidation::Valid,
			"export is not an importable file");
		return bytes;
	}

	size_t payloadDifference(const Sysex& _a, const Sysex& _b)
	{
		if(_a.size() != _b.size())
			return std::max(_a.size(), _b.size());
		size_t count = 0;
		for(size_t i = 10; i + 5 < _a.size(); ++i)
			count += _a[i] != _b[i];
		return count;
	}

	void tap(md::Device& _device, Runner& _runner, const md::PanelControl _control)
	{
		const auto packet = md::panelPacket(_device.getModel(), _control);
		require(packet.has_value(), "missing panel control");
		require(_device.sendPanelEvent(packet->row, packet->mask), "panel press rejected");
		_runner.seconds(0.125);
		require(_device.sendPanelEvent(packet->row, 0), "panel release rejected");
		_runner.seconds(0.5);
	}
}

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const bool mm = argc == 4 && std::string(argv[1]) == "mm";
	if(!(argc == 3 && std::string(argv[1]) == "md") && !mm)
	{
		std::puts("usage: mdSysexDumpExportFirmwareTest md <MD-ROM> | mm <MM-ROM> <1MiB-patch-ram>");
		return 2;
	}
	try
	{
		const auto model = mm ? md::MachineModel::Monomachine : md::MachineModel::Machinedrum;
		synthLib::DeviceCreateParams params;
		params.romData = load(argv[2]);
		params.romName = argv[2];
		params.customData = md::deviceCustomData(model);
		// No homePath: never load or overwrite the user's machine storage.
		const auto patchRam = mm ? load(argv[3]) : Sysex{};
		auto device = std::make_unique<md::Device>(params, patchRam);
		require(device->isValid(), "firmware did not create a valid device");
		Runner runner(*device);
		const auto bootDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
		while(!device->getHardware().isFirmwareMidiReady())
		{
			runner.run(512);
			require(std::chrono::steady_clock::now() < bootDeadline, "boot timed out");
		}
		runner.seconds(20);

		md::SysexDumpExportProgress progress;
		const auto all = runExport(*device, runner, md::SysexDumpScope::AllUserData, false, progress);
		require(splitSysex(all).size() == (mm ? 8u + 128 + 128 + 24 : 8u + 64 + 128 + 32), "unexpected dump count");

		const auto storedKit = runExport(*device, runner, md::SysexDumpScope::CurrentKit, false, progress);
		const auto kitSlot = progress.slot;
		const auto storedPattern = runExport(*device, runner, md::SysexDumpScope::CurrentPattern, false, progress);
		const auto patternSlot = progress.slot;

		if(!mm)
		{
			// Unsaved edits: track 1 machine parameters are CC 16.. on the base channel.
			std::vector<synthLib::SMidiEvent> edits;
			for(uint8_t cc = 16; cc < 24; ++cc)
				edits.emplace_back(synthLib::MidiEventSource::Host, 0xb0, cc, 0x11);
			runner.run(64, edits);
			runner.seconds(2);

			const auto withoutSave = runExport(*device, runner, md::SysexDumpScope::CurrentKit, false, progress);
			const auto withoutDiff = payloadDifference(storedKit, withoutSave);
			const auto withSave = runExport(*device, runner, md::SysexDumpScope::CurrentKit, true, progress);
			const auto withDiff = payloadDifference(storedKit, withSave);
			std::printf("KIT slot=%u unsavedEdits: exportWithoutSaveDiffers=%zu exportWithSaveDiffers=%zu\n",
				kitSlot, withoutDiff, withDiff);
			require(progress.slot == kitSlot, "save-first export used a different slot");
			require(withoutDiff == 0, "plain export unexpectedly included unsaved edits");
			require(withDiff > 0, "save-first export did not include the edits");

			// Pattern edits: add a trig in grid recording, then compare the dump.
			tap(*device, runner, md::PanelControl::Exit);
			tap(*device, runner, md::PanelControl::Record);
			tap(*device, runner, md::PanelControl::Trigger3);
			tap(*device, runner, md::PanelControl::Record);
			runner.seconds(1);
			const auto editedPattern = runExport(*device, runner, md::SysexDumpScope::CurrentPattern, false, progress);
			std::printf("PATTERN slot=%u afterGridEdit differingBytes=%zu -> %s\n", patternSlot,
				payloadDifference(storedPattern, editedPattern),
				payloadDifference(storedPattern, editedPattern)
					? "pattern dumps include live edits" : "pattern dump unchanged (edit not included or not made)");
		}
		std::puts("SYSEX DUMP EXPORT passed");
		return 0;
	}
	catch(const std::exception& _error)
	{
		std::fprintf(stderr, "SYSEX DUMP EXPORT FAIL: %s\n", _error.what());
		return 1;
	}
}
