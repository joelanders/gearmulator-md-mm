#include "mdLib/mdturbomidi.h"
#include "mdLib/mdsim.h"
#include "dsp56kEmu/memory.h"

#include <cstdint>
#include <iostream>
#include <initializer_list>
#include <vector>

namespace
{
	constexpr uint64_t g_testClockHz = 1000;

	class MidiSink final : public md::MidiByteSink
	{
	public:
		bool tryWriteMidiByte(const uint8_t _byte) override
		{
			bytes.push_back(_byte);
			return true;
		}

		size_t queuedMidiByteCount() const override { return 0; }

		std::vector<uint8_t> bytes;
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
		md::TurboMidiTransfer transfer(g_testClockHz, {});
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x01, 0xf7});
		if(!check(prepared.has_value(), "valid SysEx was rejected")
			|| !check(transfer.start(*prepared, 0), "transfer did not start"))
			return false;

		transfer.service(g_testClockHz, true, sink);
		const auto request = turboMessage(0x10);
		if(!check(sink.bytes == request, "capability request is malformed"))
			return false;

		transfer.service(g_testClockHz + 1, true, sink);
		const auto progress = transfer.progress();
		return check(progress.state == md::MidiSysexTransferState::Complete,
			"fallback transfer did not complete")
			&& check(progress.sent == 3, "fallback payload byte count is wrong")
			&& check(progress.fallbackReason
				== md::MidiTurboFallbackReason::CapabilityRequestTimedOut,
				"fallback reason is wrong");
	}

	bool testDocumentedHandshake()
	{
		md::TurboMidiTransfer transfer(g_testClockHz, {});
		MidiSink sink;
		auto prepared = md::prepareMidiSysexTransfer({0xf0, 0x02, 0xf7});
		if(!check(prepared.has_value(), "valid SysEx was rejected")
			|| !check(transfer.start(*prepared, 0), "transfer did not start"))
			return false;

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
			&& check(progress.sent == 3, "TurboMIDI payload byte count is wrong")
			&& check(progress.fallbackReason == md::MidiTurboFallbackReason::None,
				"TurboMIDI transfer unexpectedly fell back");
	}

	bool testDspMemoryFallback()
	{
		dsp56k::DefaultMemoryValidator validator;
		dsp56k::Memory memory(validator, 16, 16, 16);
		if(!check(!memory.hasMmuSupport(), "fallback memory unexpectedly uses an MMU mapping"))
			return false;

		memory.set(dsp56k::MemArea_Y, 3, 0x123456);
		return check(memory.get(dsp56k::MemArea_Y, 3) == 0x123456,
			"fallback memory did not preserve the written value")
			&& check(memory.get(dsp56k::MemArea_X, 3) == 0,
				"fallback memory did not preserve separate memory areas");
	}

	bool testMk2PortAInvertedLoopback()
	{
		md::Sim sim;
		sim.write8(md::Sim::g_ppddr, 0x04);
		sim.setMk2PortAInvertedLoopback(true);
		sim.write8(md::Sim::g_ppdat, 0x04);
		if(!check((sim.read8(md::Sim::g_ppdat) & 0x01) == 0,
			"MKII Port A loopback did not invert HIGH"))
			return false;
		sim.write8(md::Sim::g_ppdat, 0x00);
		return check((sim.read8(md::Sim::g_ppdat) & 0x01) != 0,
			"MKII Port A loopback did not invert LOW");
	}

	bool testFirmwareUpdateHandoff()
	{
		md::Sim mdHandoff;
		mdHandoff.prepareFirmwareUpdateBoot(false);
		if(!check(mdHandoff.read8(0x047) == 0x25
			&& mdHandoff.read8(0x06e) == 0x19
			&& mdHandoff.read8(md::Sim::g_uart1Base + md::Sim::g_uartBg2) == 0x28
			&& mdHandoff.getParallelDirection() == 0x01
			&& mdHandoff.getParallelData() == 0xfe,
			"Machinedrum update handoff did not restore SIM/UART/GPIO state"))
			return false;

		md::Sim mmHandoff;
		mmHandoff.prepareFirmwareUpdateBoot(true);
		return check(mmHandoff.read8(0x047) == 0x00
			&& mmHandoff.read8(0x06e) == 0x15
			&& mmHandoff.read8(0x0aa) == 0x01
			&& mmHandoff.read8(md::Sim::g_uart2Base + md::Sim::g_uartBg2) == 0x08
			&& mmHandoff.getParallelDirection() == 0x01
			&& mmHandoff.getParallelData() == 0xfe,
			"Monomachine update handoff did not restore SIM/UART/GPIO state");
	}
}

int main()
{
	if(!testTimeoutFallback() || !testDocumentedHandshake()
		|| !testDspMemoryFallback() || !testMk2PortAInvertedLoopback()
		|| !testFirmwareUpdateHandoff())
		return 1;
	std::cout << "mdLib tests passed\n";
	return 0;
}
