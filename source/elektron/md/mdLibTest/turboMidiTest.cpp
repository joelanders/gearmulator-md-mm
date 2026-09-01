#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdhostaudioqueue.h"
#include "mdLib/mdrealtimemidiqueue.h"
#include "mdLib/mdhardware.h"
#include "mdLib/mdplusdrive.h"
#include "mdLib/mdsim.h"
#include "mdLib/mdturbomidi.h"
#include "mdLib/mdstate.h"
#include "baseLib/filesystem.h"
#include "dsp56kEmu/memory.h"

#include <array>
#include <chrono>
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

		auto embeddedStatus = mmMessage;
		embeddedStatus.insert(embeddedStatus.end() - 1, {0x90, 0x3c, 0x7f});
		if(!check(md::validateMidiSysexStream(embeddedStatus,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::InvalidFraming,
			"embedded channel status was accepted as SysEx data"))
			return false;
		auto statusAsCommand = mmMessage;
		statusAsCommand[6] = 0x90;
		if(!check(md::validateMidiSysexStream(statusAsCommand,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::InvalidFraming,
			"MIDI status was accepted as the device command"))
			return false;
		auto nestedSysex = mmMessage;
		nestedSysex.insert(nestedSysex.end() - 1,
			mdMessage.begin(), mdMessage.end());
		if(!check(md::validateMidiSysexStream(nestedSysex,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::InvalidFraming,
			"nested SysEx framing was accepted"))
			return false;
		auto realtime = mmMessage;
		realtime.insert(realtime.end() - 1, 0xfe);
		if(!check(md::validateMidiSysexStream(realtime,
				md::MachineModel::Monomachine) == MidiSysexStreamValidation::Valid,
			"legal MIDI realtime data was rejected inside SysEx"))
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

	bool testHostAudioInputLookAhead()
	{
		md::HostAudioQueue<2, 16> queue;
		const md::HostAudioQueue<2, 16>::Frame silence{};
		for(uint32_t i = 0; i < 3; ++i)
			queue.push(silence);

		const std::array<float, 8> left{
			0.01f, 0.02f, 0.03f, 0.04f, 0.05f, 0.06f, 0.07f, 0.08f};
		const std::array<float, 8> right{
			-0.01f, -0.02f, -0.03f, -0.04f, -0.05f, -0.06f, -0.07f, -0.08f};
		const synthLib::TAudioInputs inputs{left.data(), right.data(), nullptr, nullptr};
		md::appendHostAudioInput(queue, inputs, left.size(), 0, 4);

		md::HostAudioQueue<2, 16>::Frame frame{};
		for(uint32_t i = 0; i < 3; ++i)
		{
			if(!check(queue.pop(frame) && frame[0] == 0 && frame[1] == 0,
				"host input look-ahead did not begin with silence"))
				return false;
		}
		for(uint32_t sample = 0; sample < 2; ++sample)
		{
			if(!check(queue.pop(frame)
				&& frame[0] == dsp56k::sample2dsp(left[sample])
				&& frame[1] == dsp56k::sample2dsp(right[sample]),
				"host input changed during simulated scheduler overshoot"))
				return false;
		}

		md::appendHostAudioInput(queue, inputs, left.size(), 4, 4);
		for(uint32_t sample = 2; sample < left.size(); ++sample)
		{
			if(!check(queue.pop(frame)
				&& frame[0] == dsp56k::sample2dsp(left[sample])
				&& frame[1] == dsp56k::sample2dsp(right[sample]),
				"host input continuity was lost across callback blocks"))
				return false;
		}
		return check(queue.empty(), "host input queue retained unexpected samples");
	}

	bool testHostAudioOutputRouting()
	{
		dsp56k::Audio::TxFrame codecFrame;
		codecFrame.resize(2);
		codecFrame[0] = {10, 20, 30};
		codecFrame[1] = {11, 21, 31};
		md::RealtimeHostAudioQueue::Frame mappedFrame{};
		md::mapCodecOutputFrame(mappedFrame, codecFrame);
		if(!check(mappedFrame == md::RealtimeHostAudioQueue::Frame{
			10, 11, 20, 21, 30, 31},
			"codec slots and transmitters were mapped to the wrong host channels"))
			return false;

		codecFrame.resize(1);
		md::mapCodecOutputFrame(mappedFrame, codecFrame);
		if(!check(mappedFrame == md::RealtimeHostAudioQueue::Frame{
			10, 0, 20, 0, 30, 0},
			"missing codec slot did not map to silent host channels"))
			return false;

		md::HostAudioQueue<6, 8> queue;
		const auto pushFrame = [&queue](const dsp56k::TWord _sample)
		{
			queue.emplace([_sample](md::HostAudioQueue<6, 8>::Frame& _frame)
			{
				for(size_t channel = 0; channel < _frame.size(); ++channel)
					_frame[channel] = _sample + static_cast<dsp56k::TWord>(channel * 100);
			});
		};
		pushFrame(1);
		pushFrame(2);

		std::array<std::vector<dsp56k::TWord>, 6> outputs;
		for(auto& output : outputs)
			output.resize(5);
		dsp56k::TWord generated = 3;
		md::renderHostAudio(queue, outputs, 5, [&](const uint32_t _frames)
		{
			for(uint32_t i = 0; i < _frames; ++i)
				pushFrame(generated++);
		});

		for(size_t channel = 0; channel < outputs.size(); ++channel)
		{
			for(dsp56k::TWord frameIndex = 0; frameIndex < 5; ++frameIndex)
			{
				if(!check(outputs[channel][frameIndex]
					== frameIndex + 1 + static_cast<dsp56k::TWord>(channel * 100),
					"host output channel mapping or carry order is wrong"))
					return false;
			}
		}
		return check(queue.size() == 2,
			"host output queue did not preserve scheduler surplus");
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
			|| !check(publisher.tryPublish(panel, 456),
				"front-panel snapshot publication failed"))
			return false;
		const auto published = publisher.readPublishedState();
		if(!check(published.ledSequence == beforePublish.producedSequence
				&& publisher.getLedTransitionStatus().publishedSequence
					== published.ledSequence,
			"front-panel snapshot did not capture its LED sequence")
			|| !check(published.emulationCycles == 456,
				"front-panel snapshot did not capture its emulation time"))
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
		const auto overflowStatus = publisher.getLedTransitionStatus();
		if(!check(publisher.readPublishedState().ledSequence
				< overflowStatus.producedSequence,
			"stale snapshot unexpectedly covered a dropped transition")
			|| !check(publisher.tryPublish(panel, 10000),
				"post-overflow front-panel snapshot publication failed")
			|| !check(publisher.readPublishedState().ledSequence
				== overflowStatus.producedSequence,
			"post-overflow snapshot did not cover the recovery target"))
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

	bool testUwSparseProjectState()
	{
		std::vector<uint8_t> patchRam(md::g_patchRamStateSize);
		for(size_t i = 0; i < patchRam.size(); ++i)
			patchRam[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xffu);
		std::vector<uint8_t> rom(md::g_romSize, 0xff);
		for(size_t i = 0; i < rom.size(); i += 4093)
			rom[i] = static_cast<uint8_t>(i >> 8);
		auto factoryBaseline = rom;
		factoryBaseline[md::g_uwFlashSectorSize + 7] = 0x61;

		std::vector<uint8_t> factoryState;
		if(!check(md::encodeState(factoryState, patchRam, factoryBaseline,
			factoryBaseline, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal),
			"factory-only UW state could not be encoded")
			|| !check(factoryState.size() == 52 + patchRam.size(),
				"factory initialization leaked into project state"))
			return false;
		std::vector<uint8_t> romEqualFactoryState;
		if(!check(md::encodeStateWithFactoryBaseline(romEqualFactoryState, patchRam,
			rom, rom, rom, md::MachineModel::Machinedrum,
			synthLib::StateTypeGlobal),
			"ROM-equal validated factory state could not be encoded")
			|| !check(romEqualFactoryState.size() == 52 + patchRam.size(),
				"ROM-equal validated factory state expanded to a complete flash image"))
			return false;

		auto flashA = factoryBaseline;
		flashA[3 * md::g_uwFlashSectorSize + 19] = 0x18;
		flashA.back() = 0x00;
		md::PlusDrive plusDriveA;
		auto plusDriveImageA = plusDriveA.copyStorage();
		// Add one sparse sector to the canonical MDPD image.
		plusDriveImageA[15] = 1;
		plusDriveImageA.insert(plusDriveImageA.end(), {0, 0, 0, 7});
		plusDriveImageA.insert(plusDriveImageA.end(), 512, 0x5a);
		if(!check(plusDriveA.replaceStorage(plusDriveImageA),
			"test +Drive image is invalid"))
			return false;

		std::vector<uint8_t> encodedA;
		if(!check(md::encodeState(encodedA, patchRam, flashA, factoryBaseline, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal,
			plusDriveImageA),
			"UW sparse state could not be encoded"))
			return false;

		constexpr size_t expectedChangedSectors = 2;
		constexpr size_t stateHeaderSize = 60;
		constexpr size_t stateHeaderSizeWithoutPlusDrive = 52;
		constexpr size_t sectorEntryHeaderSize = 8;
		const auto expectedSize = stateHeaderSize + patchRam.size()
			+ expectedChangedSectors
				* (sectorEntryHeaderSize + md::g_uwFlashSectorSize)
			+ plusDriveImageA.size();
		if(!check(encodedA.size() == expectedSize,
			"UW sparse state did not contain exactly the changed sectors"))
			return false;

		// Exercise a meaningfully populated sparse drive without allocating the
		// documented worst-case 128 sample banks in every CI run. Serialization is
		// exactly 516 bytes per occupied 512-byte sector, so this 16K-sector fixture
		// also verifies the size calculation used for larger project files.
		constexpr uint32_t populatedSectorCount = 16u * 1024u;
		std::vector<uint8_t> populatedDrive{
			'M', 'D', 'P', 'D', 0, 0, 0, 1, 0, 0, 2, 0,
			0, 0, static_cast<uint8_t>(populatedSectorCount >> 8), 0};
		populatedDrive.reserve(16u + static_cast<size_t>(populatedSectorCount) * 516u);
		for(uint32_t sector = 0; sector < populatedSectorCount; ++sector)
		{
			populatedDrive.push_back(static_cast<uint8_t>(sector >> 24));
			populatedDrive.push_back(static_cast<uint8_t>(sector >> 16));
			populatedDrive.push_back(static_cast<uint8_t>(sector >> 8));
			populatedDrive.push_back(static_cast<uint8_t>(sector));
			populatedDrive.insert(populatedDrive.end(), 512,
				static_cast<uint8_t>(sector));
		}
		std::vector<uint8_t> populatedState;
		md::DecodedState decodedPopulated;
		const auto populatedExpectedSize = stateHeaderSize + patchRam.size()
			+ expectedChangedSectors
				* (sectorEntryHeaderSize + md::g_uwFlashSectorSize)
			+ populatedDrive.size();
		if(!check(md::PlusDrive::validateStorage(populatedDrive),
			"populated +Drive fixture was invalid")
			|| !check(md::encodeState(populatedState, patchRam, flashA,
				factoryBaseline, rom, md::MachineModel::Machinedrum,
				synthLib::StateTypeGlobal, populatedDrive),
				"populated +Drive state could not be encoded")
			|| !check(populatedState.size() == populatedExpectedSize,
				"populated +Drive state size was not linear and exact")
			|| !check(md::decodeState(decodedPopulated, populatedState, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
				"populated +Drive state could not be decoded")
			|| !check(decodedPopulated.plusDrive == populatedDrive,
				"populated +Drive state changed its storage bytes"))
			return false;

		md::DecodedState decodedA;
		std::vector<uint8_t> restoredA;
		if(!check(md::decodeState(decodedA, encodedA, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW sparse state could not be decoded")
			|| !check(decodedA.containsFlash, "UW state lost its flash marker")
			|| !check(decodedA.containsPlusDrive,
				"UW state lost its +Drive marker")
			|| !check(decodedA.patchRam == patchRam, "UW state changed patch RAM")
			|| !check(decodedA.plusDrive == plusDriveImageA,
				"UW state changed +Drive data")
			|| !check(md::applyFlashOverlay(restoredA, decodedA.flashOverlay,
				factoryBaseline), "UW sparse state could not be applied")
			|| !check(restoredA == flashA, "UW state changed flash data"))
			return false;

		// A second instance must reconstruct its own flash, not inherit A's sectors.
		auto flashB = factoryBaseline;
		flashB[7 * md::g_uwFlashSectorSize + 11] = 0x77;
		auto plusDriveImageB = plusDriveImageA;
		plusDriveImageB[19] = 9;
		plusDriveImageB[20] = 0x27;
		std::vector<uint8_t> encodedB;
		md::DecodedState decodedB;
		std::vector<uint8_t> restoredB;
		if(!check(md::encodeState(encodedB, patchRam, flashB, factoryBaseline, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal,
			plusDriveImageB),
			"second UW state could not be encoded")
			|| !check(md::decodeState(decodedB, encodedB, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
				"second UW state could not be decoded")
			|| !check(md::applyFlashOverlay(restoredB, decodedB.flashOverlay,
				factoryBaseline), "second UW state could not be applied")
			|| !check(restoredB == flashB && restoredB != restoredA,
				"UW instances did not retain isolated flash images")
			|| !check(decodedB.plusDrive == plusDriveImageB
				&& decodedB.plusDrive != decodedA.plusDrive,
				"UW instances did not retain isolated +Drive images"))
			return false;

		auto replacedDriveState = encodedA;
		md::DecodedState decodedReplacement;
		if(!check(md::replacePlusDriveState(replacedDriveState, plusDriveImageB),
			"project +Drive payload could not be replaced")
			|| !check(md::decodeState(decodedReplacement, replacedDriveState, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
				"state with replaced +Drive could not be decoded")
			|| !check(decodedReplacement.plusDrive == plusDriveImageB,
				"state replacement did not install the requested +Drive")
			|| !check(decodedReplacement.flashOverlay.data == decodedA.flashOverlay.data
				&& decodedReplacement.patchRam == decodedA.patchRam,
				"+Drive replacement changed machine or UW flash state"))
			return false;
		auto rejectedReplacement = replacedDriveState;
		const std::vector<uint8_t> invalidDrive{1, 2, 3};
		if(!check(!md::replacePlusDriveState(rejectedReplacement, invalidDrive),
			"invalid +Drive replacement was accepted")
			|| !check(rejectedReplacement == replacedDriveState,
				"failed +Drive replacement modified project state"))
			return false;

		auto wrongRom = rom;
		wrongRom[123] ^= 1;
		md::DecodedState unchanged;
		unchanged.patchRam = {1, 2, 3};
		if(!check(!md::decodeState(unchanged, encodedA, wrongRom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state accepted the wrong ROM")
			|| !check(unchanged.patchRam == std::vector<uint8_t>({1, 2, 3}),
				"failed UW decode modified its destination"))
			return false;
		auto wrongFactory = factoryBaseline;
		wrongFactory[456] ^= 1;
		if(!check(!md::applyFlashOverlay(restoredA, decodedA.flashOverlay, wrongFactory),
			"UW state accepted the wrong factory baseline"))
			return false;

		// Before a machine-local factory cache exists, state carries every sector.
		// In particular, a factory-populated sector erased back to ROM bytes is an
		// intentional deletion and must override a fresh factory initialization.
		auto firstRunFlash = factoryBaseline;
		std::copy_n(rom.begin() + md::g_uwFlashSectorSize,
			md::g_uwFlashSectorSize,
			firstRunFlash.begin() + md::g_uwFlashSectorSize);
		firstRunFlash[9 * md::g_uwFlashSectorSize + 31] = 0x27;
		std::vector<uint8_t> firstRunState;
		md::DecodedState decodedFirstRun;
		std::vector<uint8_t> restoredFirstRun;
		if(!check(md::encodeState(firstRunState, patchRam, firstRunFlash, rom, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"first-run UW fallback state could not be encoded")
			|| !check(firstRunState.size() == stateHeaderSizeWithoutPlusDrive + patchRam.size()
				+ (md::g_romSize / md::g_uwFlashSectorSize)
					* (sectorEntryHeaderSize + md::g_uwFlashSectorSize),
				"first-run UW fallback did not contain a complete flash image")
			|| !check(md::decodeState(decodedFirstRun, firstRunState, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
				"first-run UW fallback state could not be decoded")
			|| !check(md::applyFlashOverlay(restoredFirstRun,
				decodedFirstRun.flashOverlay, factoryBaseline),
				"complete UW fallback was not baseline-independent")
			|| !check(restoredFirstRun == firstRunFlash,
				"first-run UW fallback lost a ROM-equal deletion"))
			return false;

		auto corrupt = encodedA;
		corrupt.back() ^= 1;
		if(!check(!md::decodeState(unchanged, corrupt, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state accepted corrupt flash data"))
			return false;
		auto truncated = encodedA;
		truncated.pop_back();
		if(!check(!md::decodeState(unchanged, truncated, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"UW state accepted truncated +Drive data"))
			return false;

		// Version 3 remains valid and intentionally has no project-owned +Drive.
		std::vector<uint8_t> version3;
		md::DecodedState decodedVersion3;
		if(!check(md::encodeState(version3, patchRam, flashA, factoryBaseline, rom,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"version-3 MD state could not be encoded")
			|| !check(md::decodeState(decodedVersion3, version3, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
				"version-3 MD state could not be decoded")
			|| !check(!decodedVersion3.containsPlusDrive,
				"version-3 MD state unexpectedly replaced +Drive"))
			return false;
		const auto originalVersion3 = version3;
		if(!check(!md::replacePlusDriveState(version3, plusDriveImageA),
			"version-3 MD state was ambiguously upgraded in place")
			|| !check(version3 == originalVersion3,
				"failed version-3 replacement modified the legacy state"))
			return false;

		// Version-1 states remain valid and intentionally inherit the live flash.
		std::vector<uint8_t> legacy;
		md::DecodedState decodedLegacy;
		return check(md::encodeState(legacy, patchRam,
			md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
			"legacy MD state could not be encoded")
			&& check(md::decodeState(decodedLegacy, legacy, rom,
				md::MachineModel::Machinedrum, synthLib::StateTypeGlobal),
				"legacy MD state could not be decoded")
			&& check(!decodedLegacy.containsFlash && !decodedLegacy.containsPlusDrive
				&& decodedLegacy.patchRam == patchRam,
				"legacy MD state unexpectedly replaced flash");
	}

	bool testUwFactoryFlashCache()
	{
		std::vector<uint8_t> baseline(md::g_romSize, 0xff);
		baseline[0] = 0x12;
		baseline[md::g_romSize - 1] = 0x34;
		auto initialized = baseline;
		initialized[2 * md::g_uwFlashSectorSize + 9] = 0x56;

		std::vector<uint8_t> cache;
		std::vector<uint8_t> decoded;
		if(!check(md::encodeFactoryFlashCache(cache, initialized, baseline),
			"UW factory cache could not be encoded")
			|| !check(cache.size() == 36 + 8 + md::g_uwFlashSectorSize,
				"UW factory cache did not contain exactly one sparse sector")
			|| !check(cache.size() < md::g_romSize,
				"UW factory cache copied the complete ROM image")
			|| !check(md::decodeFactoryFlashCache(decoded, cache, baseline),
				"UW factory cache could not be decoded")
			|| !check(decoded == initialized, "UW factory cache changed flash data"))
			return false;

		auto wrongRom = baseline;
		wrongRom[1234] ^= 1;
		decoded = {1, 2, 3};
		if(!check(!md::decodeFactoryFlashCache(decoded, cache, wrongRom),
			"UW factory cache accepted the wrong ROM")
			|| !check(decoded == std::vector<uint8_t>({1, 2, 3}),
				"failed factory-cache decode modified its destination"))
			return false;

		auto corrupt = cache;
		corrupt.back() ^= 1;
		return check(!md::decodeFactoryFlashCache(decoded, corrupt, baseline),
			"UW factory cache accepted corrupt flash data");
	}

	bool testCachePromotionAndRecovery()
	{
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto filename = baseLib::filesystem::getCurrentDirectory()
			+ ".md-cache-exclusive-test-" + std::to_string(nonce);
		const std::vector<uint8_t> first{1, 2, 3, 4};
		const std::vector<uint8_t> second{9, 8, 7};
		const std::vector<uint8_t> recovered{5, 6, 7, 8, 9};
		std::vector<uint8_t> readback;
		const bool firstCreated = baseLib::filesystem::writeFileExclusive(filename, first);
		const bool secondRejected = !baseLib::filesystem::writeFileExclusive(filename, second);
		const bool invalidReplaced = baseLib::filesystem::writeFileAtomic(filename, recovered);
		const bool read = baseLib::filesystem::readFile(readback, filename);
		baseLib::filesystem::remove(filename);
		return check(firstCreated, "immutable cache was not created")
			&& check(secondRejected, "immutable cache was replaced")
			&& check(invalidReplaced, "invalid cache could not be replaced atomically")
			&& check(read && readback == recovered,
				"atomic cache recovery changed replacement data");
	}
}

int main()
{
	if(!testTimeoutFallback() || !testDocumentedHandshake() || !testModelValidation()
		|| !testDspMemoryFallback() || !testMk2PortAInvertedLoopback()
		|| !testRealtimeMidiPendingState() || !testHostAudioInputLookAhead()
		|| !testHostAudioOutputRouting() || !testFrontPanelStepLeds()
		|| !testFrontPanelLedTransitions()
		|| !testFirmwareUpdateHandoff()
		|| !testUwSparseProjectState()
		|| !testUwFactoryFlashCache() || !testCachePromotionAndRecovery())
		return 1;
	std::cout << "mdLib tests passed\n";
	return 0;
}
