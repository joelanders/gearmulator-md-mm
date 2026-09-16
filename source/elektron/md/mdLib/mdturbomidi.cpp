#include "mdturbomidi.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "synthLib/midiTypes.h"

namespace md
{
	namespace
	{
		using TurboCommand = turboMidi::Command;

		namespace policy = turboMidi::senderPolicy;
	}

	TurboMidiTransfer::TurboMidiTransfer(const uint64_t _clockHz)
		: m_clockHz(_clockHz)
	{
	}

	void TurboMidiTransfer::publishProgress()
	{
		m_progress.publish({m_state.load(std::memory_order_relaxed), m_sent,
			m_total, m_speedCode, m_speedCode > 1, m_fallbackReason,
			m_fallbackCount, m_error, m_retries, m_acknowledgedSamples, m_serviceSerial,
			m_transferId, m_messageIndex,
			m_messageIndex < m_messages.size() ? m_messages[m_messageIndex].kind : m_receiveKind});
	}

	bool TurboMidiTransfer::start(PreparedMidiSysexTransfer& _transfer,
		const size_t _realtimeWriteBoundary)
	{
		if(_transfer.m_bytes.empty())
			return false;
		AccessGuard access(m_access);
		if(!access || ownsMidiWire())
			return false;

		// Swap rather than move-assign: the retired allocation leaves Hardware in
		// _transfer and is freed by the caller after releasing the plug-in lock.
		m_payload.swap(_transfer.m_bytes);
		m_messages.swap(_transfer.m_messages);
		m_model = _transfer.m_model;
		m_receiveKind = m_messages.front().kind;
		static std::atomic<uint32_t> nextId{0};
		m_transferId = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
		m_messageIndex = 0;
		m_sdsActive = m_sdsWaiting = false;
		m_sdsPacket = m_packetRetries = 0;
		m_retries = m_acknowledgedSamples = m_serviceSerial = 0;
		m_sdsWaitCycles = 0;
		m_error = MidiSysexTransferError::None;
		m_payloadCursor = 0;
		m_total = m_payload.size();
		m_sent = 0;
		m_realtimeWriteBoundary = _realtimeWriteBoundary;
		m_wire.clear();
		m_baudAccumulator = 0;
		m_phase = Phase::BeginNegotiation;
		m_negotiatedSpeeds = {1, 1};
		m_speedCode = 1;
		m_bytesPerSecond = turboMidi::Speeds[1].bytesPerSecond;
		m_phaseCycles = 0;
		m_activeSenseCycles = 0;
		m_fallbackReason = MidiTurboFallbackReason::None;
		m_fallbackCount = 0;
		m_partialResponse.clear();
		clearResponses();
		m_transmitBytes.clear();
		m_captureTransmit = false;
		m_state.store(MidiSysexTransferState::Queued, std::memory_order_release);
		m_observedOverflow = m_overflow.load(std::memory_order_relaxed);
		publishProgress();
		return true;
	}

	bool TurboMidiTransfer::cancel(std::vector<uint8_t>& _retiredPayload)
	{
		AccessGuard access(m_access);
		if(!access || !ownsMidiWire() || !_retiredPayload.empty())
			return false;

		// Payload ownership leaves the scheduler immediately, but destruction is
		// deferred to the control-plane caller after it releases the Plugin lock.
		_retiredPayload.swap(m_payload);
		m_payloadCursor = 0;
		abortPayload(MidiSysexTransferError::None);
		publishProgress();
		return true;
	}

	void TurboMidiTransfer::abortPayload(const MidiSysexTransferError _error)
	{
		m_error = _error;
		m_captureTransmit = false;
		m_transmitBytes.clear();
		m_partialResponse.clear();
		clearResponses();
		m_wire.clear();
		m_activeSenseCycles = 0;
		m_baudAccumulator = m_clockHz;
		m_phaseCycles = 0;
		// A prefix of the current SysEx may already be buffered in the emulated
		// UART. Append EOX behind it before releasing the wire so subsequent MIDI
		// cannot become part of a truncated message.
		(void)m_wire.tryPush(0xf7);
		if(m_sdsActive)
		{
			const uint8_t cancel[] = {0xf0, 0x7e, 0x00, 0x7d, m_sdsPacket, 0xf7};
			(void)m_wire.tryPush(cancel, std::size(cancel));
		}
		m_phase = Phase::DrainCancellation;
		m_state.store(MidiSysexTransferState::Cancelling,
			std::memory_order_release);
	}

	bool TurboMidiTransfer::retirePayload(std::vector<uint8_t>& _retiredPayload)
	{
		AccessGuard access(m_access);
		if(!access || ownsMidiWire() || !_retiredPayload.empty() || m_payload.empty())
			return false;
		_retiredPayload.swap(m_payload);
		m_payloadCursor = 0;
		return true;
	}

	bool TurboMidiTransfer::resumeReceiveMode(uint32_t _transferId, size_t _step)
	{
		AccessGuard access(m_access);
		if(!access || m_phase != Phase::WaitReceiveMode
			|| m_transferId != _transferId || m_messageIndex != _step) return false;
		m_receiveKind = m_messages[m_messageIndex].kind;
		m_phase = m_model == MachineModel::Machinedrum ? Phase::WaitFallbackReset : Phase::Payload;
		m_phaseCycles = 0;
		m_state.store(MidiSysexTransferState::Sending, std::memory_order_release);
		publishProgress();
		return true;
	}

	void TurboMidiTransfer::observeTransmitByte(const uint8_t _byte)
	{
		AccessGuard access(m_access);
		if(!access)
		{
			m_overflow.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		if(m_captureTransmit && !m_transmitBytes.tryPush(_byte))
			m_overflow.fetch_add(1, std::memory_order_relaxed);
	}

	void TurboMidiTransfer::queueMessage(const turboMidi::Command _command,
		const std::initializer_list<uint8_t> _payload)
	{
		const auto& header = turboMidi::Header;
		std::array<uint8_t, 32> message{};
		const size_t size = std::size(header) + 1 + _payload.size() + 1;
		if(size > message.size())
		{
			++m_overflow;
			return;
		}
		auto output = std::copy(std::begin(header), std::end(header), message.begin());
		*output++ = static_cast<uint8_t>(_command);
		output = std::copy(_payload.begin(), _payload.end(), output);
		*output = 0xf7;
		if(!m_wire.tryPush(message.data(), size))
			++m_overflow;
	}

	void TurboMidiTransfer::parseTransmitBytes()
	{
		uint8_t byte = 0;
		while(m_transmitBytes.tryPop(byte))
		{
			if(byte >= 0xf8 && byte != 0xf7)
				continue;
			if(byte == 0xf0)
			{
				m_partialResponse.clear();
				m_partialResponse.bytes[m_partialResponse.size++] = byte;
				continue;
			}
			if(m_partialResponse.empty())
				continue;
			if(m_partialResponse.size >= Response::Capacity)
			{
				// Unsolicited dump replies are not handshake-queue overflows.
				// Drop the rest of this message and resynchronize on the next F0.
				m_partialResponse.clear();
				continue;
			}
			m_partialResponse.bytes[m_partialResponse.size++] = byte;
			if(byte != 0xf7)
				continue;

			const auto& message = m_partialResponse;
			if(message.size == 6 && message[1] == 0x7e && message[2] == 0
				&& message[3] >= 0x7c && message[3] <= 0x7f && message[4] < 0x80
				&& !pushResponse(message))
				++m_overflow;
			if(message.size >= turboMidi::EnvelopeBytes
				&& std::equal(turboMidi::Header.begin(), turboMidi::Header.end(), message.bytes.begin())
				&& !pushResponse(message))
				++m_overflow;
			m_partialResponse.clear();
		}
	}

	bool TurboMidiTransfer::pushResponse(const Response& _message)
	{
		if(m_responseCount >= m_responses.size())
			return false;
		const auto write = (m_responseRead + m_responseCount) % m_responses.size();
		m_responses[write] = _message;
		++m_responseCount;
		return true;
	}

	void TurboMidiTransfer::clearResponses()
	{
		m_responseRead = 0;
		m_responseCount = 0;
	}

	bool TurboMidiTransfer::takeResponse(const turboMidi::Command _command, Response& _message)
	{
		// Match command IDs after framing; preserve tolerance for trailing data.
		// Only the speed report and first test result inspect their payloads.
		while(m_responseCount > 0)
		{
			auto& candidate = m_responses[m_responseRead];
			m_responseRead = (m_responseRead + 1) % m_responses.size();
			--m_responseCount;
			if(candidate.size >= turboMidi::EnvelopeBytes
				&& candidate[turboMidi::CommandOffset] == static_cast<uint8_t>(_command))
			{
				_message = candidate;
				return true;
			}
		}
		return false;
	}

	void TurboMidiTransfer::beginPayload()
	{
		m_phase = Phase::Payload;
		m_phaseCycles = 0;
		m_captureTransmit = true;
		clearResponses();
		m_state.store(MidiSysexTransferState::Sending, std::memory_order_release);
	}

	bool TurboMidiTransfer::takeSdsResponse(Response& _message)
	{
		while(m_responseCount)
		{
			const auto candidate = m_responses[m_responseRead];
			m_responseRead = (m_responseRead + 1) % m_responses.size();
			--m_responseCount;
			// If an ACK was lost, MD 1.63 answers the retransmitted packet
			// with NAK for the next packet it wants. This is not a bad ACK:
			// honor that request only after a retry, within the same sample.
			const auto& current = m_messages[m_messageIndex];
			const bool requestsNext = candidate[3] == 0x7e && m_packetRetries
				&& current.kind == MidiSysexMessageKind::SdsPacket && !current.lastSamplePacket
				&& candidate[4] == ((m_sdsPacket + 1) & 0x7f);
			if(candidate.size == 6 && candidate[1] == 0x7e && candidate[2] == 0
				&& (candidate[4] == m_sdsPacket || requestsNext))
			{
				_message = candidate;
				return true;
			}
		}
		return false;
	}

	void TurboMidiTransfer::retrySds()
	{
		if(m_packetRetries >= 3)
		{
			abortPayload(MidiSysexTransferError::RetryLimit);
			return;
		}
		++m_packetRetries;
		++m_retries;
		m_sdsWaiting = false;
		m_sdsWaitCycles = 0;
		m_phaseCycles = 0;
		clearResponses();
		m_payloadCursor = m_messages[m_messageIndex].offset;
		m_phase = Phase::Payload;
		m_state.store(MidiSysexTransferState::Retrying, std::memory_order_release);
	}

	void TurboMidiTransfer::serviceSds()
	{
		Response response;
		while(takeSdsResponse(response))
		{
			if(response[3] == 0x7d)
			{
				abortPayload(MidiSysexTransferError::DeviceCancelled);
				return;
			}
			if(response[3] == 0x7c)
			{
				m_sdsWaiting = true;
				continue;
			}
			if(response[3] == 0x7e && response[4] == m_sdsPacket)
			{
				retrySds();
				return;
			}
			if(response[3] == 0x7f || response[3] == 0x7e)
			{
				const bool sampleFinished = m_messages[m_messageIndex].lastSamplePacket;
				if(sampleFinished)
				{
					++m_acknowledgedSamples;
					m_sdsActive = false;
				}
				++m_messageIndex;
				m_sdsWaiting = false;
				m_packetRetries = 0;
				m_sdsWaitCycles = 0;
				m_phase = Phase::Payload;
				m_phaseCycles = 0;
				clearResponses();
				m_state.store(MidiSysexTransferState::Sending, std::memory_order_release);
				if(sampleFinished && m_messageIndex < m_messages.size()
					&& m_messages[m_messageIndex].kind != MidiSysexMessageKind::SdsHeader)
				{
					// ACK means received, not that CLEANING/LOADING has finished.
					// Consecutive samples are a supported firmware workflow; other
					// data must wait for the user's readiness confirmation. Let the
					// Turbo link reset while firmware completes the sample bank.
					m_speedCode = 1;
					m_bytesPerSecond = turboMidi::Speeds[1].bytesPerSecond;
					m_baudAccumulator = 0;
					m_phase = Phase::WaitReceiveMode;
					m_state.store(MidiSysexTransferState::WaitingForReceiveMode, std::memory_order_release);
				}
				return;
			}
		}
		// Explicit WAIT holds the transfer, but cannot wedge the instrument forever.
		// These are emulated-time deadlines, unaffected by host suspension.
		if(m_sdsWaiting)
		{
			if(m_sdsWaitCycles >= m_clockHz * 30)
				abortPayload(MidiSysexTransferError::ReplyTimedOut);
		}
		else if(m_phaseCycles >= m_clockHz * 2)
			retrySds();
	}

	void TurboMidiTransfer::fallBack(const bool _waitForPeerReset,
		const MidiTurboFallbackReason _reason)
	{
		m_fallbackReason = _reason;
		++m_fallbackCount;
		m_captureTransmit = false;
		m_wire.clear();
		clearResponses();
		m_partialResponse.clear();
		m_speedCode = 1;
		m_bytesPerSecond = turboMidi::Speeds[1].bytesPerSecond;
		m_activeSenseCycles = 0;
		m_baudAccumulator = 0;
		m_phaseCycles = 0;
		if(_waitForPeerReset)
			m_phase = Phase::WaitFallbackReset;
		else
			beginPayload();
	}

	void TurboMidiTransfer::pumpWire(MidiByteSink& _midiPort)
	{
		const uint64_t activeSenseIntervalCycles = (m_clockHz * policy::ActiveSenseMilliseconds) / 1000u;
		bool blocked = false;
		while(m_baudAccumulator >= m_clockHz)
		{
			bool admitted = false;
			if(m_speedCode > 1 && m_activeSenseCycles >= activeSenseIntervalCycles)
			{
				if(!_midiPort.tryWriteMidiByte(synthLib::M_ACTIVESENSING))
				{
					blocked = true;
					break;
				}
				m_activeSenseCycles -= activeSenseIntervalCycles;
				admitted = true;
			}
			else if(!m_wire.empty())
			{
				uint8_t byte = 0;
				if(!m_wire.tryPeek(byte))
					break;
				if(!_midiPort.tryWriteMidiByte(byte))
				{
					blocked = true;
					break;
				}
				uint8_t committed = 0;
				if(!m_wire.tryPop(committed))
					break;
				admitted = true;
			}
			else if(m_phase == Phase::Payload && m_payloadCursor < m_payload.size())
			{
				const auto& message = m_messages[m_messageIndex];
				if(m_payloadCursor == message.offset)
				{
					if(m_model == MachineModel::Monomachine && message.kind != m_receiveKind)
					{
						m_phase = Phase::DrainReceiveMode;
						break;
					}
					if(message.kind == MidiSysexMessageKind::SdsHeader)
					{
						m_sdsActive = true;
						m_sdsPacket = 0;
					}
					else if(message.kind == MidiSysexMessageKind::SdsPacket)
						m_sdsPacket = m_payload[message.offset + 4];
				}
				if(!_midiPort.tryWriteMidiByte(m_payload[m_payloadCursor]))
				{
					blocked = true;
					break;
				}
				++m_payloadCursor;
				m_sent = std::max(m_sent, m_payloadCursor);
				if(m_payloadCursor == message.offset + message.size)
				{
					if(message.kind == MidiSysexMessageKind::SdsHeader
						|| message.kind == MidiSysexMessageKind::SdsPacket)
						m_phase = Phase::DrainSds;
					else
						++m_messageIndex;
				}
				admitted = true;
			}

			if(!admitted)
				break;
			m_baudAccumulator -= m_clockHz;
		}

		if(!blocked && m_wire.empty()
			&& (m_phase != Phase::Payload || m_payloadCursor >= m_payload.size()))
			m_baudAccumulator = std::min<uint64_t>(m_baudAccumulator, m_clockHz - 1u);
	}

	void TurboMidiTransfer::setBytePacing(const uint8_t _code)
	{
		m_speedCode = _code;
		m_bytesPerSecond = turboMidi::Speeds[_code].bytesPerSecond;
		m_baudAccumulator = 0;
	}

	// Negotiation transcript: request/report -> negotiate/ack -> padding and
	// first test/result -> second test/result -> firmware settle -> payload.
	// Current host admission rate / observed MD 1.63 & MM 1.32b UART divider:
	//   0x10/11, 0x12/13: 1x / 1x (fresh negotiation)
	//   padding, 0x14/15: speed1 / speed1
	//   0x16/17:          speed2 / speed1
	//   after 0x17:       speed2 / speed2, with a 10ms host settling pause
	// The peer observation is at UTB writes, not at a physical final stop bit.
	// See mdTurboMidiFirmwareTest for the independent peer-register check.
	// Send phases end at admission; payload/SDS drain phases also wait for the
	// sink's pending-byte count to reach zero. See MidiByteSink for its meaning.
	void TurboMidiTransfer::serviceNegotiation()
	{
		const uint64_t responseTimeoutCycles = m_clockHz * policy::ResponseTimeoutSeconds;
		const uint64_t fallbackResetCycles = (m_clockHz * policy::PeerResetMilliseconds) / 1000u;
		Response response;
		switch(m_phase)
		{
		case Phase::BeginNegotiation:
			m_captureTransmit = true;
			queueMessage(TurboCommand::SpeedRequest);
			m_phase = Phase::SendSpeedRequest;
			m_phaseCycles = 0;
			m_state.store(MidiSysexTransferState::NegotiatingTurbo,
				std::memory_order_release);
			break;
		case Phase::WaitSpeedReport:
			if(takeResponse(TurboCommand::SpeedReport, response))
			{
				if(response.size < turboMidi::SpeedReportBytes)
				{
					fallBack(false, MidiTurboFallbackReason::MalformedSpeedAnswer);
					break;
				}
				const auto* data = response.bytes.data() + turboMidi::DataOffset;
				const auto supported = policy::decodeCapabilityMask(data[0], data[1]);
				const auto certified = policy::decodeCapabilityMask(data[2], data[3]);
				m_negotiatedSpeeds = policy::selectSpeeds(supported, certified);
				if(!m_negotiatedSpeeds.turboAvailable())
				{
					fallBack(false, MidiTurboFallbackReason::NoCommonCertifiedSpeed);
					break;
				}
				queueMessage(TurboCommand::SpeedNegotiation,
					{m_negotiatedSpeeds.firstTest, m_negotiatedSpeeds.transfer});
				m_phase = Phase::SendNegotiation;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(false, MidiTurboFallbackReason::CapabilityRequestTimedOut);
			break;
		case Phase::WaitSpeedAcknowledgement:
			if(takeResponse(TurboCommand::SpeedAcknowledgement, response))
			{
				setBytePacing(m_negotiatedSpeeds.firstTest);
				m_activeSenseCycles = 0;
				for(uint32_t i = 0; i < turboMidi::FirstTestPaddingBytes; ++i)
					if(!m_wire.tryPush(0x00))
						++m_overflow;
				queueMessage(TurboCommand::FirstTest, turboMidi::FirstTestPattern);
				m_phase = Phase::SendFirstTest;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(false, MidiTurboFallbackReason::SpeedAcknowledgementTimedOut);
			break;
		case Phase::WaitFirstTestResult:
			if(takeResponse(TurboCommand::FirstTestResult, response))
			{
				const auto& expected = turboMidi::FirstTestPattern;
				const bool valid = response.size >= turboMidi::EnvelopeBytes + expected.size()
					&& std::equal(std::begin(expected), std::end(expected),
						response.bytes.begin() + turboMidi::DataOffset);
				if(!valid)
				{
					fallBack(true, MidiTurboFallbackReason::FirstLinkTestBadData);
					break;
				}
				// Retained host admission policy: speed2 applies before 0x16.
				// This is NOT the peer's UART baud boundary: MD 1.63/MM 1.32b
				// keep the speed1 divider through their 0x17 UTB write, then
				// set speed2 (mdTurboMidiFirmwareTest). UART1 does not model
				// baud mismatch, so this passing exchange is not a DIN test.
				setBytePacing(m_negotiatedSpeeds.transfer);
				queueMessage(TurboCommand::SecondTest);
				m_phase = Phase::SendSecondTest;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(true, MidiTurboFallbackReason::FirstLinkTestTimedOut);
			break;
		case Phase::WaitSecondTestResult:
			if(takeResponse(TurboCommand::SecondTestResult, response))
			{
				// The reply observer sees EOX before firmware has returned from its
				// final speed-change/UART-reset routine. MD 1.63 drops an immediate
				// SDS header here. Give that transition 10 ms of emulated time.
				m_phase = Phase::SettleLink;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(true, MidiTurboFallbackReason::SecondLinkTestTimedOut);
			break;
		case Phase::SettleLink:
			if(m_phaseCycles >= m_clockHz / 100)
				beginPayload();
			break;
		case Phase::WaitFallbackReset:
			if(m_phaseCycles >= fallbackResetCycles)
				beginPayload();
			break;
		default:
			break;
		}
	}

	void TurboMidiTransfer::finishNegotiationSend()
	{
		const auto enterWait = [this](const Phase _send, const Phase _wait)
		{
			if(m_phase == _send && m_wire.empty())
			{
				m_phase = _wait;
				m_phaseCycles = 0;
			}
		};
		enterWait(Phase::SendSpeedRequest, Phase::WaitSpeedReport);
		enterWait(Phase::SendNegotiation, Phase::WaitSpeedAcknowledgement);
		enterWait(Phase::SendFirstTest, Phase::WaitFirstTestResult);
		enterWait(Phase::SendSecondTest, Phase::WaitSecondTestResult);
	}

	void TurboMidiTransfer::service(const uint32_t _cycles,
		const bool _ingressDrained, MidiByteSink& _midiPort)
	{
		if(!ownsMidiWire())
			return;
		AccessGuard access(m_access);
		if(!access || !ownsMidiWire() || !_ingressDrained)
			return;

		parseTransmitBytes();
		const auto overflow = m_overflow.load(std::memory_order_relaxed);
		if(m_sdsActive && overflow != m_observedOverflow)
			abortPayload(MidiSysexTransferError::ResponseOverflow);
		m_observedOverflow = overflow;
		++m_serviceSerial;
		m_phaseCycles += _cycles;
		if(m_phase == Phase::WaitSds) m_sdsWaitCycles += _cycles;
		if(m_speedCode > 1)
			m_activeSenseCycles += _cycles;

		// A single phase is serviced before pumping bytes. Newly entered wait
		// phases cannot consume replies until the next scheduler call.
		if(m_phase == Phase::WaitSds)
			serviceSds();
		else
			serviceNegotiation();

		m_baudAccumulator += static_cast<uint64_t>(_cycles) * m_bytesPerSecond;
		pumpWire(_midiPort);

		finishNegotiationSend();
		if(m_phase == Phase::DrainSds && _midiPort.queuedMidiByteCount() == 0)
		{
			m_phase = Phase::WaitSds;
			m_phaseCycles = 0;
			m_state.store(MidiSysexTransferState::WaitingForDevice, std::memory_order_release);
		}
		if(m_phase == Phase::DrainReceiveMode && _midiPort.queuedMidiByteCount() == 0)
		{
			m_phase = Phase::WaitReceiveMode;
			m_state.store(MidiSysexTransferState::WaitingForReceiveMode, std::memory_order_release);
		}

		if(m_phase == Phase::Payload && m_payloadCursor >= m_payload.size())
		{
			m_speedCode = 1;
			m_bytesPerSecond = turboMidi::Speeds[1].bytesPerSecond;
			m_baudAccumulator = 0;
			m_phase = Phase::DrainPayload;
		}
		if(m_phase == Phase::DrainPayload && _midiPort.queuedMidiByteCount() == 0)
		{
			m_captureTransmit = false;
			m_phase = Phase::Idle;
			m_state.store(MidiSysexTransferState::Complete,
				std::memory_order_release);
		}
		if(m_phase == Phase::DrainCancellation && m_wire.empty()
			&& _midiPort.queuedMidiByteCount() == 0)
		{
			m_phase = Phase::Idle;
			m_speedCode = 1;
			m_bytesPerSecond = turboMidi::Speeds[1].bytesPerSecond;
			m_state.store(m_error == MidiSysexTransferError::None
				? MidiSysexTransferState::Cancelled : MidiSysexTransferState::Failed,
				std::memory_order_release);
		}
		publishProgress();
	}
}
