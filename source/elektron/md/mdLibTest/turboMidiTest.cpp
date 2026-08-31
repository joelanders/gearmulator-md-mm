#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdrealtimemidiqueue.h"
#include "mdLib/mdsim.h"
#include "mdLib/mdturbomidi.h"
#include "dsp56kEmu/memory.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
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

	bool testModelValidation()
	{
		using md::MidiSysexStreamValidation;
		const std::vector<uint8_t> mdMessage{
			0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x52, 0x01, 0xf7};
		const std::vector<uint8_t> mmMessage{
			0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x5d, 0x01, 0xf7};
		const std::vector<uint8_t> mmOsUpdate{
			0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x7e, 0x00, 0xf7};
		if(!check(md::validateMidiSysexStream(mdMessage,
				md::MachineModel::Machinedrum) == MidiSysexStreamValidation::Valid,
			"Machinedrum user-data stream was rejected")
			|| !check(md::validateMidiSysexStream(mmMessage,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::Valid,
			"Monomachine user-data stream was rejected")
			|| !check(md::validateMidiSysexStream(mdMessage,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::WrongModel,
			"wrong-model stream was accepted")
			|| !check(md::validateMidiSysexStream(mmOsUpdate,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::FirmwareUpdate,
			"OS updater was accepted as user data"))
			return false;

		auto concatenated = mmMessage;
		concatenated.insert(concatenated.end(), mmMessage.begin(), mmMessage.end());
		if(!check(md::validateMidiSysexStream(concatenated,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::Valid,
			"concatenated user-data messages were rejected"))
			return false;
		concatenated.push_back(0x00);
		return check(md::validateMidiSysexStream(concatenated,
			md::MachineModel::Monomachine) == MidiSysexStreamValidation::InvalidFraming,
			"trailing non-SysEx data was accepted");
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

	bool testRealtimeMidiPendingState()
	{
		md::RealtimeMidiByteQueue<4> queue;
		const std::array<uint8_t, 2> input{0x90, 0x40};
		if(!check(!queue.hasPending(), "new realtime MIDI queue is not empty")
			|| !check(queue.tryPush(input), "realtime MIDI push failed")
			|| !check(queue.hasPending(), "published realtime MIDI was not visible"))
			return false;

		uint8_t output = 0;
		return check(queue.tryPop(output) && output == input[0],
			"first realtime MIDI byte is wrong")
			&& check(queue.hasPending(), "remaining realtime MIDI was not visible")
			&& check(queue.tryPop(output) && output == input[1],
				"second realtime MIDI byte is wrong")
			&& check(!queue.hasPending(), "drained realtime MIDI queue is not empty");
	}

	bool testFrontPanelStepLeds()
	{
		bool mmMappingMatches = true;
		for(uint32_t step = 0; step < 16; ++step)
		{
			md::FrontPanel panel;
			const uint8_t command = static_cast<uint8_t>(
				md::FrontPanel::g_firstLedBank + (step >> 2));
			const uint8_t bit = static_cast<uint8_t>(((step & 3) << 1) + 1);
			const uint8_t message[] = {command,
				static_cast<uint8_t>(~static_cast<uint8_t>(1u << bit))};
			panel.processBytes(message, sizeof(message));
			for(uint32_t candidate = 0; candidate < 16; ++candidate)
				mmMappingMatches &= panel.getMonomachineStepLed(candidate)
					== (candidate == step);
		}
		if(!check(mmMappingMatches, "Monomachine step LED mapping is wrong"))
			return false;

		md::FrontPanel panel;
		const uint8_t mdMessage[] = {0x20, 0xfe, 0x21, 0x7f};
		panel.processBytes(mdMessage, sizeof(mdMessage));
		return check(panel.getStepLed(0) && panel.getStepLed(15),
			"Machinedrum step LED mapping changed")
			&& check(!panel.getStepLed(1) && !panel.getStepLed(14),
				"Machinedrum unlit step decoding changed")
			&& check(!panel.getStepLed(16) && !panel.getMonomachineStepLed(16),
				"out-of-range step LED was accepted");
	}

	bool testFrontPanelLedTransitions()
	{
		md::FrontPanel panel;
		if(!check(!panel.processByte(0x22).has_value(),
			"LED command byte completed a transition"))
			return false;
		const auto tempoOn = panel.processByte(0xfd);
		if(!check(tempoOn.has_value() && tempoOn->command == 0x22
				&& tempoOn->value == 0xfd,
			"completed tempo LED transition was not reported"))
			return false;
		(void)panel.processByte(0x22);
		if(!check(!panel.processByte(0xfd).has_value(),
			"unchanged LED bank produced a transition"))
			return false;

		md::FrontPanelPublisher publisher;
		if(!check(publisher.tryPushLedTransition(0x22, 0xfd, 100),
			"first LED transition was rejected")
			|| !check(publisher.tryPushLedTransition(0x22, 0xff, 120),
				"second LED transition was rejected"))
			return false;
		const auto beforePublish = publisher.getLedTransitionStatus();
		if(!check(beforePublish.producedSequence == 2
				&& beforePublish.publishedSequence == 0,
			"LED snapshot sequence advanced before publication")
			|| !check(publisher.tryPublish(panel),
				"front-panel snapshot publication failed"))
			return false;
		const auto published = publisher.readPublishedState();
		if(!check(published.ledSequence == beforePublish.producedSequence
				&& publisher.getLedTransitionStatus().publishedSequence
					== published.ledSequence,
			"front-panel snapshot did not capture its LED sequence"))
			return false;

		std::array<md::FrontPanelLedTransition, 4> output;
		const auto count = publisher.drainLedTransitions(output.data(), output.size());
		if(!check(count == 2, "LED transition queue returned the wrong count")
			|| !check(output[0].sequence + 1 == output[1].sequence,
				"LED transition sequence is discontinuous")
			|| !check(output[0].emulationCycles == 100 && output[0].value == 0xfd,
				"first LED transition payload is wrong")
			|| !check(output[1].emulationCycles == 120 && output[1].value == 0xff,
				"second LED transition payload is wrong"))
			return false;

		for(size_t i = 0; i < md::FrontPanelPublisher::g_ledTransitionCapacity; ++i)
			if(!check(publisher.tryPushLedTransition(0x22,
				static_cast<uint8_t>(i), i), "LED transition queue filled early"))
				return false;
		if(!check(!publisher.tryPushLedTransition(0x22, 0xff, 9999),
			"full LED transition queue accepted an event")
			|| !check(publisher.getLedTransitionStatus().dropped == 1,
				"LED transition overflow was not reported"))
			return false;
		std::array<md::FrontPanelLedTransition,
			md::FrontPanelPublisher::g_ledTransitionCapacity> backlog;
		if(!check(publisher.drainLedTransitions(backlog.data(), backlog.size())
				== backlog.size(), "full LED transition queue did not drain")
			|| !check(publisher.tryPushLedTransition(0x26, 0x7f, 10000),
				"wrapped LED transition queue rejected an event")
			|| !check(publisher.drainLedTransitions(output.data(), output.size()) == 1
				&& output[0].command == 0x26 && output[0].emulationCycles == 10000,
				"wrapped LED transition queue corrupted its event"))
			return false;

		const auto epoch = publisher.getLedTransitionStatus().epoch;
		publisher.reset();
		return check(publisher.getLedTransitionStatus().epoch == epoch + 1,
			"LED transition reset did not advance the epoch")
			&& check(publisher.getLedTransitionStatus().dropped == 0,
				"LED transition reset retained overflow state")
			&& check(publisher.drainLedTransitions(output.data(), output.size()) == 0,
				"LED transition reset retained queued events");
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
	if(!testTimeoutFallback() || !testDocumentedHandshake() || !testModelValidation()
		|| !testDspMemoryFallback() || !testMk2PortAInvertedLoopback()
		|| !testRealtimeMidiPendingState() || !testFrontPanelStepLeds()
		|| !testFrontPanelLedTransitions()
		|| !testFirmwareUpdateHandoff())
		return 1;
	std::cout << "mdLib tests passed\n";
	return 0;
}
