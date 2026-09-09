#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdscheduledmidi.h"
#include "baseLib/filesystem.h"
#include "synthLib/plugin.h"
#include "synthLib/midiBufferParser.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace md
{
	struct MidiTimingTestAccess
	{
		static void frame(Hardware& _hardware, uint64_t _frame)
		{
			_hardware.m_schedFramesTotal = static_cast<double>(_frame);
		}
		static void pump(Hardware& _hardware, uint64_t _cycle)
		{
			_hardware.m_schedUcCyclesDone = _cycle;
			_hardware.pumpScheduledMidi();
			_hardware.pumpMidiIngress();
		}
		static size_t pending(const Hardware& _hardware) { return _hardware.m_scheduledMidi.size(); }
		static uint64_t overflow(const Hardware& _hardware) { return _hardware.m_scheduledMidiOverflow; }
		static void restorePending(Hardware& _hardware, bool _pending)
		{
			_hardware.m_pendingFlashRestoreActive.store(_pending);
		}
		static void instruction(Hardware& _hardware) { _hardware.processUC(); }
	};
}

namespace
{
	using synthLib::SMidiEvent;
	using synthLib::MidiEventSource;
	using Access = md::MidiTimingTestAccess;

	void require(bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void queueBoundaries()
	{
		md::ScheduledMidiQueue<4> queue;
		// Equal-time messages retain wire order; unsorted input is chronological.
		require(queue.push({MidiEventSource::Host, 0x90, 61, 100}, 200), "push failed");
		require(queue.push({MidiEventSource::Host, 0x80, 61, 0}, 200), "push failed");
		require(queue.push({MidiEventSource::Internal, 0xfa}, 100), "push failed");
		require(queue.push({MidiEventSource::Internal, 0xfc}, 300), "push failed");
		require(!queue.push({MidiEventSource::Host, 0xb0, 1, 20}, 50), "queue exceeded its bound");
		require(!queue.ready(99) && queue.ready(100), "event became ready before its deadline");
		for(auto status : {0xfa, 0x90, 0x80, 0xfc})
		{
			require(queue.front().event.a == status, "MIDI ordering changed");
			queue.pop();
		}
		require(queue.empty(), "queue failed to drain");
		require(queue.push({MidiEventSource::Host, 0xf8}, uint64_t{1} << 40), "reuse failed");
		require(!queue.ready((uint64_t{1} << 40) - 1), "deadline truncated to 32 bits");
		require(queue.ready(uint64_t{1} << 40), "64-bit event did not become ready");
	}

	void conversion()
	{
		// Independent quotient oracle is safe for this bounded 48-hour sweep.
		for(uint64_t sample = 0; sample < 44100ull * 86400 * 2; sample += 7919)
		{
			const auto expected = (sample * 40000000 + 44099) / 44100;
			require(md::midiReceiveDeadline<40000000, 44100>(sample) == expected,
				"native sample deadline rounded incorrectly");
		}
		require(md::midiReceiveDeadline<40000000, 44100>(0) == 0, "zero origin moved");
		require(md::midiReceiveDeadline<40000000, 44100>(1) == 908, "fraction rounded early");
		constexpr auto maximum = std::numeric_limits<uint64_t>::max();
		require(md::midiReceiveDeadline<40000000, 44100>(maximum) == maximum,
			"unrepresentable future deadline wrapped");
	}

	void parserTimestamps()
	{
		synthLib::MidiBufferParser parser(MidiEventSource::Device);
		std::vector<SMidiEvent> output;
		parser.write(0x90,61); parser.write(60,62);
		parser.getEvents(output);
		require(output.empty(), "incomplete message was emitted");
		// A new callback has a new relative origin. A real-time tick can appear
		// inside the pending note; each receives its own completion timestamp.
		parser.write(0xf8,1); parser.write(100,7);
		parser.write(61,12); parser.write(0,17); // running-status note
		parser.write(0xf0,24); parser.write(1,26); parser.write(0xf8,28); parser.write(0xf7,31);
		parser.getEvents(output);
		require(output.size()==5, "timestamp parsing changed event count");
		const std::array<unsigned,5> offsets{1,7,17,28,31};
		for(unsigned i=0;i<offsets.size();++i)
			require(output[i].offset==offsets[i], "byte completion timestamp was lost");
		require(output[0].a==0xf8 && output[1].a==0x90 && output[1].b==60
			&& output[2].b==61 && output[3].a==0xf8
			&& output[4].sysex==synthLib::SysexBuffer({0xf0,1,0xf7}),
			"timestamp parsing changed message contents");
	}

	void uartOutputTimestamp(md::Device& _device)
	{
		auto& hardware=_device.getHardware();
		auto& uc=hardware.getUC();
		std::vector<SMidiEvent> output;
		uc.readMidiOut(output);
		output.clear();
		while(uc.getCycles()<20000) Access::instruction(hardware);
		const auto before=uc.getCycles();
		uc.getSim().write8(md::Sim::g_uart1Base+md::Sim::g_uartRxTx,0xf8);
		const auto absolute=(before*44100+39999999)/40000000;
		uc.readMidiOut(output,absolute-3);
		require(output.size()==1 && output[0].a==0xf8 && output[0].offset==3,
			"UART output lost its emulated cycle timestamp or native block origin");
	}

	std::vector<uint8_t> drain(md::Hardware& _hardware, bool _panel)
	{
		auto& sim = _hardware.getUC().getSim();
		const auto uart = _panel ? md::Sim::g_uartPanel : md::Sim::g_uartMidi;
		const auto base = _panel ? md::Sim::g_uart2Base : md::Sim::g_uart1Base;
		std::vector<uint8_t> bytes;
		while(sim.queuedRxBytes(uart))
			bytes.push_back(sim.read8(base + md::Sim::g_uartRxTx));
		return bytes;
	}

	void deviceAdmission(md::Device& _device, bool _panel, uint32_t _offset,
		uint32_t _extraLatency, uint64_t _frame)
	{
		auto& hardware = _device.getHardware();
		Access::frame(hardware, _frame);
		_device.setExtraLatencySamples(_extraLatency);
		const SMidiEvent note(MidiEventSource::Host, 0x90, _panel ? 36 : 60, 100, _offset);
		std::vector<SMidiEvent> output;
		// The real Device translation/routing path admits events before a native
		// block. A zero-length block isolates admission without running firmware.
		_device.process({}, {}, 0, {note}, output);
		const auto at = _frame + _offset + _extraLatency;
		const auto expected = (at * 40000000 + 44099) / 44100;
		require(expected > 0, "test must exercise a nonzero deadline");
		Access::pump(hardware, expected - 1);
		require(drain(hardware, false).empty() && drain(hardware, true).empty(),
			"Device delivered MIDI/pad input before its sample deadline");
		require(Access::pending(hardware) == 1, "future event was lost");
		Access::pump(hardware, expected);
		const std::vector<uint8_t> bytes = _panel ? std::vector<uint8_t>{0x20, 1, 0x20, 0}
			: std::vector<uint8_t>{0x90, 60, 100};
		require(drain(hardware, _panel) == bytes, "deadline delivery changed UART bytes");
		require(drain(hardware, !_panel).empty(), "event reached the wrong UART");
		require(Access::pending(hardware) == 0, "delivered event was retained");
	}

	void latencyTransitions(synthLib::Plugin& plugin, md::Device& device)
	{
		auto& hardware = device.getHardware();
		const auto cycle = [](uint64_t sample) { return (sample * 40000000 + 44099) / 44100; };
		for(const auto frame : {uint64_t{0}, uint64_t{44100} * 86400 * 2})
			for(unsigned change = 0; change < 5; ++change)
			{
				plugin.setHostSamplerate(44100, 44100);
				plugin.setBlockSize(512);
				plugin.setLatencyBlocks(change == 1 ? 0 : 1);
				Access::frame(hardware, frame);
				Access::pump(hardware, cycle(frame));
				std::vector<SMidiEvent> output;
				// Admit through the real Device translator/router, then change the
				// real Plugin control while both messages are still scheduled.
				device.process({}, {}, 0, {
					{MidiEventSource::Host, 0x90, 60, 100, 63},
					{MidiEventSource::Internal, 0xfa, 0, 0, 63}}, output);
				Access::frame(hardware, frame + 32);
				switch(change)
				{
				case 0: plugin.setLatencyBlocks(0); break;
				case 1: plugin.setLatencyBlocks(1); break;
				case 2: plugin.setBlockSize(128); break;
				case 3: plugin.setHostSamplerate(96000, 44100); break;
				case 4:
					plugin.setLatencyBlocks(0);
					plugin.setLatencyBlocks(8);
					plugin.setLatencyBlocks(0);
					break;
				}
				const auto delay = device.getExtraLatencySamples();
				Access::frame(hardware, frame + 128);
				device.process({}, {}, 0, {
					{MidiEventSource::Host, 0x80, 60, 0},
					{MidiEventSource::Internal, 0xfc}}, output);
				const auto on = cycle(frame + 63 + delay), off = cycle(frame + 128 + delay);
				Access::pump(hardware, on - 1);
				require(drain(hardware, false).empty(), "retimed MIDI was early");
				Access::pump(hardware, on);
				require(drain(hardware, false) == std::vector<uint8_t>({0x90,60,100,0xfa}),
					"latency change stranded Note On/Start behind newer Note Off/Stop");
				Access::pump(hardware, off - 1);
				require(drain(hardware, false).empty(), "retimed Note Off/Stop was early");
				Access::pump(hardware, off);
				require(drain(hardware, false) == std::vector<uint8_t>({0x80,60,0,0xfc})
					&& Access::pending(hardware) == 0, "latency change lost Note Off/Stop");
			}
		// Decreasing the delay can make several pending events overdue. They
		// drain chronologically, including equal-time wire order, at the next pump.
		plugin.setHostSamplerate(44100, 44100);
		plugin.setBlockSize(512);
		plugin.setLatencyBlocks(1);
		Access::frame(hardware, 0);
		std::vector<SMidiEvent> output;
		device.process({}, {}, 0, {
			{MidiEventSource::Host, 0x90, 60, 100}, {MidiEventSource::Internal, 0xfa},
			{MidiEventSource::Host, 0x80, 60, 0, 1}, {MidiEventSource::Internal, 0xfc, 0, 0, 1}}, output);
		Access::frame(hardware, 128);
		plugin.setLatencyBlocks(0);
		Access::pump(hardware, cycle(128));
		require(drain(hardware, false) == std::vector<uint8_t>({0x90,60,100,0xfa,0x80,60,0,0xfc})
			&& Access::pending(hardware) == 0, "overdue events reordered during a delay reduction");
		std::cout << "latency transitions passed (blocks, block size, rate, repeated changes, overdue)\n";
	}

	void orderedMessages(md::Device& _device)
	{
		auto& hardware = _device.getHardware();
		Access::frame(hardware, 0);
		_device.setExtraLatencySamples(0);
		std::vector<SMidiEvent> output;
		_device.process({}, {}, 0, {
			{MidiEventSource::Host, 0x90, 60, 90, 63},
			{MidiEventSource::Host, 0x80, 60, 0, 63},
			{MidiEventSource::Internal, 0xf2, 3, 2, 7},
			{MidiEventSource::Internal, 0xfa, 0, 0, 7},
			{MidiEventSource::Internal, 0xf8, 0, 0, 11},
			{MidiEventSource::Host, 0xb0, 1, 45, 31},
			{MidiEventSource::Internal, 0xfc, 0, 0, 64}}, output);
		const std::array<uint32_t, 5> offsets{7, 11, 31, 63, 64};
		const std::array<std::vector<uint8_t>, 5> expected{{
			{0xf2, 3, 2, 0xfa}, {0xf8}, {0xb0, 1, 45}, {0x90, 60, 90, 0x80, 60, 0}, {0xfc}}};
		for(size_t i = 0; i < offsets.size(); ++i)
		{
			const auto cycle = (uint64_t(offsets[i]) * 40000000 + 44099) / 44100;
			Access::pump(hardware, cycle - 1);
			require(drain(hardware, false).empty(), "ordered event was early");
			Access::pump(hardware, cycle);
			require(drain(hardware, false) == expected[i], "note/CC/clock/transport ordering changed");
		}
	}

	void pressureAndArbitration(md::Device& _device, bool _panel)
	{
		auto& hardware = _device.getHardware();
		Access::frame(hardware, 0);
		Access::pump(hardware, 0);
		const auto oldOverflow = Access::overflow(hardware);
		std::vector<uint8_t> expected, actual;
		for(unsigned i=0;i<16384;++i)
		{
			const uint8_t first = _panel ? 36 + i % 16 : i % 128;
			const uint8_t second = _panel ? 100 : (i / 128) % 128;
			require(hardware.scheduleMidi({MidiEventSource::Host,
				static_cast<uint8_t>(_panel ? 0x90 : 0xb0), first, second}, 0),
				"production scheduled queue filled before its advertised capacity");
			if(_panel) {
				const uint8_t row = 0x20 + (first-36)/8, mask = 1u << ((first-36)%8);
				expected.insert(expected.end(), {row,mask,row,0});
			} else expected.insert(expected.end(), {0xb0,first,second});
		}
		require(!hardware.scheduleMidi({MidiEventSource::Host,0xf8},0),
			"production scheduled queue accepted an event beyond capacity");
		require(Access::overflow(hardware) == oldOverflow+1, "overflow was not counted");
		for(unsigned n=0;n<1024 && actual.size()<expected.size();++n) {
			Access::pump(hardware,0);
			auto chunk=drain(hardware,_panel);
			actual.insert(actual.end(),chunk.begin(),chunk.end());
		}
		require(actual == expected && Access::pending(hardware) == 0,
			"UART backpressure lost, reordered or split queued events");

		// Exercise the real instruction boundary that gates input during restore.
		require(hardware.scheduleMidi({MidiEventSource::Host,0xb0,7,90},0), "queue reuse failed");
		Access::restorePending(hardware,true);
		Access::instruction(hardware);
		require(Access::pending(hardware)==1 && drain(hardware,false).empty(),
			"pending restore consumed a queued event");
		Access::restorePending(hardware,false);
		Access::pump(hardware,1000);
		require(drain(hardware,false)==std::vector<uint8_t>({0xb0,7,90}),
			"event did not survive pending restore");

		// File transfers retain exclusive ownership of the MIDI wire. A due clock
		// waits intact and is delivered after cancellation releases that ownership.
		// The file sender validates model, checksum and length before claiming
		// the wire. Cancel this minimal envelope before it reaches the firmware.
		const auto model = _device.getModel();
		const uint8_t product = model == md::MachineModel::Monomachine ? 3 : 2;
		auto transfer=md::prepareMidiSysexTransfer(
			{0xf0,0,0x20,0x3c,product,0,0x52,1,1,0,0,0,0,5,0xf7}, model);
		require(transfer && hardware.startMidiSysexTransfer(*transfer), "transfer did not start");
		require(hardware.scheduleMidi({MidiEventSource::Internal,0xf8},0), "clock queue failed");
		Access::pump(hardware,1000);
		require(drain(hardware,false).empty(), "clock interleaved into an exclusive transfer");
		std::vector<uint8_t> retired;
		require(hardware.cancelMidiSysexTransfer(retired), "transfer cancellation failed");
		Access::instruction(hardware);
		require(drain(hardware,false)==std::vector<uint8_t>({0xf7}),
			"cancelled SysEx did not close before the queued clock");
		Access::instruction(hardware); // retire after the end-of-exclusive byte drains
		Access::pump(hardware,1000);
		require(drain(hardware,false)==std::vector<uint8_t>({0xf8}),
			"clock was lost while the transfer owned the wire");
	}

	bool firmware()
	{
		for(const auto model : {md::MachineModel::Machinedrum, md::MachineModel::Monomachine})
		{
			const auto* path = std::getenv(model == md::MachineModel::Machinedrum
				? "GEARMULATOR_MD_FIRMWARE_BIN" : "GEARMULATOR_MM_FIRMWARE_BIN");
			if(!path || !*path)
				return false;
			synthLib::DeviceCreateParams params;
			params.romName = path;
			require(baseLib::filesystem::readFile(params.romData, path), "cannot read firmware");
			require(md::RomLoader::isRomForModel(params.romData, model), "unsupported firmware");
			params.customData = md::deviceCustomData(model);
			auto device = std::make_unique<md::Device>(params);
			require(device->isValid(), "invalid device");
			synthLib::Plugin plugin(device.get(), [](synthLib::Device*) {});
			plugin.setHostSamplerate(44100, 44100);
			plugin.setBlockSize(512);
			require(plugin.getLatencyBlocks() == 0 && plugin.getLatencyMidiToOutput() == 0,
				"synchronous device retained an unrealized default MIDI block");
			require(plugin.getLatencyInputToOutput() == 64, "input safety latency was lost");
			plugin.setLatencyBlocks(1);
			require(plugin.getLatencyMidiToOutput() == 512 && plugin.getLatencyInputToOutput() == 576,
				"explicit latency did not update both reports");
			for(const auto frame : {uint64_t{0}, uint64_t{44100} * 86400 * 2})
				for(const auto offset : {1u, 63u, 127u, 255u, 511u, 16385u})
					for(const auto delay : {0u, 128u, 512u})
						deviceAdmission(*device, model == md::MachineModel::Machinedrum, offset, delay, frame);
			latencyTransitions(plugin, *device);
			if(model == md::MachineModel::Monomachine)
				orderedMessages(*device);
			pressureAndArbitration(*device, model == md::MachineModel::Machinedrum);
			uartOutputTimestamp(*device);
		}
		return true;
	}
}

int main(int argc, char** argv)
{
	try
	{
		if(argc > 1 && std::string_view(argv[1]) == "--firmware")
		{
			if(!firmware())
			{
				std::cout << "Set GEARMULATOR_MD_FIRMWARE_BIN and GEARMULATOR_MM_FIRMWARE_BIN\n";
				return 77;
			}
			std::cout << "MD/MM Device deadlines, UART bytes and latency reports passed\n";
		}
		else
		{
			queueBoundaries();
			conversion();
			parserTimestamps();
			std::cout << "MIDI queue ordering, capacity and 48-hour clock conversion passed\n";
		}
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
