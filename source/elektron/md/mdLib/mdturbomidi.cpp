#include "mdturbomidi.h"

#include <algorithm>
#include <iterator>
#include <utility>

#include "synthLib/midiTypes.h"

namespace md
{
	TurboMidiTransfer::TurboMidiTransfer(const uint64_t _clockHz)
		: m_clockHz(_clockHz)
	{
	}


	void TurboMidiTransfer::publishProgress()
	{
		m_progress.publish({m_state.load(std::memory_order_relaxed), m_sent,
			m_total, m_speedCode, m_speedCode > 1, m_fallbackReason,
			m_fallbackCount});
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
		m_payloadCursor = 0;
		m_total = m_payload.size();
		m_sent = 0;
		m_realtimeWriteBoundary = _realtimeWriteBoundary;
		m_wire.clear();
		m_baudAccumulator = 0;
		m_phase = Phase::BeginNegotiation;
		m_speed1 = 1;
		m_speed2 = 1;
		m_speedCode = 1;
		m_bytesPerSecond = 3125;
		m_phaseCycles = 0;
		m_activeSenseCycles = 0;
		m_fallbackReason = MidiTurboFallbackReason::None;
		m_fallbackCount = 0;
		m_partialResponse.clear();
		clearResponses();
		m_transmitBytes.clear();
		m_captureTransmit = false;
		m_state.store(MidiSysexTransferState::Queued, std::memory_order_release);
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
		m_captureTransmit = false;
		m_transmitBytes.clear();
		m_partialResponse.clear();
		clearResponses();
		m_wire.clear();
		m_speedCode = 1;
		m_bytesPerSecond = 3125;
		m_activeSenseCycles = 0;
		m_baudAccumulator = m_clockHz;
		m_phaseCycles = 0;
		// A prefix of the current SysEx may already be buffered in the emulated
		// UART. Append EOX behind it before releasing the wire so subsequent MIDI
		// cannot become part of a truncated message.
		if(!m_wire.tryPush(0xf7))
		{
			++m_overflow;
			return false;
		}
		m_phase = Phase::DrainCancellation;
		m_state.store(MidiSysexTransferState::Cancelling,
			std::memory_order_release);
		publishProgress();
		return true;
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

	void TurboMidiTransfer::queueMessage(const uint8_t _command,
		const std::initializer_list<uint8_t> _payload)
	{
		static constexpr uint8_t header[] = {0xf0, 0x00, 0x20, 0x3c, 0x00, 0x00};
		std::array<uint8_t, 32> message{};
		const size_t size = std::size(header) + 1 + _payload.size() + 1;
		if(size > message.size())
		{
			++m_overflow;
			return;
		}
		auto output = std::copy(std::begin(header), std::end(header), message.begin());
		*output++ = _command;
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
				++m_overflow;
				m_partialResponse.clear();
				continue;
			}
			m_partialResponse.bytes[m_partialResponse.size++] = byte;
			if(byte != 0xf7)
				continue;

			const auto& message = m_partialResponse;
			if(message.size >= 8 && message[0] == 0xf0 && message[1] == 0x00
				&& message[2] == 0x20 && message[3] == 0x3c
				&& message[4] == 0x00 && message[5] == 0x00
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

	bool TurboMidiTransfer::takeResponse(const uint8_t _command, Response& _message)
	{
		while(m_responseCount > 0)
		{
			auto& candidate = m_responses[m_responseRead];
			m_responseRead = (m_responseRead + 1) % m_responses.size();
			--m_responseCount;
			if(candidate.size > 7 && candidate[6] == _command)
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
		m_captureTransmit = false;
		m_state.store(MidiSysexTransferState::Sending, std::memory_order_release);
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
		m_bytesPerSecond = 3125;
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
		const uint64_t activeSenseIntervalCycles = (m_clockHz * 150u) / 1000u;
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
				if(!_midiPort.tryWriteMidiByte(m_payload[m_payloadCursor]))
				{
					blocked = true;
					break;
				}
				++m_payloadCursor;
				++m_sent;
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

	void TurboMidiTransfer::service(const uint32_t _cycles,
		const bool _ingressDrained, MidiByteSink& _midiPort)
	{
		if(!ownsMidiWire())
			return;
		AccessGuard access(m_access);
		if(!access || !ownsMidiWire() || !_ingressDrained)
			return;

		parseTransmitBytes();
		m_phaseCycles += _cycles;
		if(m_speedCode > 1)
			m_activeSenseCycles += _cycles;

		const uint64_t responseTimeoutCycles = m_clockHz;
		const uint64_t fallbackResetCycles = (m_clockHz * 350u) / 1000u;
		Response response;
		switch(m_phase)
		{
		case Phase::BeginNegotiation:
			m_captureTransmit = true;
			queueMessage(0x10);
			m_phase = Phase::SendRequest;
			m_phaseCycles = 0;
			m_state.store(MidiSysexTransferState::NegotiatingTurbo,
				std::memory_order_release);
			break;
		case Phase::WaitAnswer:
			if(takeResponse(0x11, response))
			{
				if(response.size < 12)
				{
					fallBack(false, MidiTurboFallbackReason::MalformedSpeedAnswer);
					break;
				}
				const uint16_t supported = static_cast<uint16_t>(response[7])
					| (static_cast<uint16_t>(response[8]) << 7);
				const uint16_t certified = static_cast<uint16_t>(response[9])
					| (static_cast<uint16_t>(response[10]) << 7);
				const auto highestCode = [](const uint16_t _mask)
				{
					for(int bit = 7; bit >= 0; --bit)
						if(_mask & (uint16_t{1} << bit))
							return static_cast<uint8_t>(bit + 1);
					return uint8_t{1};
				};
				const uint16_t common = supported & 0x00ffu;
				m_speed1 = highestCode(common);
				const uint16_t speed1Bit = uint16_t{1} << (m_speed1 - 1);
				m_speed2 = (certified & speed1Bit) ? m_speed1
					: highestCode(common & (speed1Bit - 1u));
				if(m_speed1 <= 1 || m_speed2 <= 1)
				{
					fallBack(false, MidiTurboFallbackReason::NoCommonCertifiedSpeed);
					break;
				}
				queueMessage(0x12, {m_speed1, m_speed2});
				m_phase = Phase::SendNegotiation;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(false, MidiTurboFallbackReason::CapabilityRequestTimedOut);
			break;
		case Phase::WaitAck:
			if(takeResponse(0x13, response))
			{
				static constexpr uint32_t speeds[] =
					{3125, 3125, 6250, 10406, 12500, 15625, 20812, 25000, 31250};
				m_speedCode = m_speed1;
				m_bytesPerSecond = speeds[m_speedCode];
				m_baudAccumulator = 0;
				m_activeSenseCycles = 0;
				for(uint32_t i = 0; i < 16; ++i)
					if(!m_wire.tryPush(0x00))
						++m_overflow;
				queueMessage(0x14,
					{0x55, 0x55, 0x55, 0x55, 0x00, 0x00, 0x00, 0x00});
				m_phase = Phase::SendTest1;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(false, MidiTurboFallbackReason::SpeedAcknowledgementTimedOut);
			break;
		case Phase::WaitTest1:
			if(takeResponse(0x15, response))
			{
				static constexpr uint8_t expected[] =
					{0x55, 0x55, 0x55, 0x55, 0x00, 0x00, 0x00, 0x00};
				const bool valid = response.size >= 16
					&& std::equal(std::begin(expected), std::end(expected),
						response.bytes.begin() + 7);
				if(!valid)
				{
					fallBack(true, MidiTurboFallbackReason::FirstLinkTestBadData);
					break;
				}
				static constexpr uint32_t speeds[] =
					{3125, 3125, 6250, 10406, 12500, 15625, 20812, 25000, 31250};
				m_speedCode = m_speed2;
				m_bytesPerSecond = speeds[m_speedCode];
				m_baudAccumulator = 0;
				queueMessage(0x16);
				m_phase = Phase::SendTest2;
				m_phaseCycles = 0;
			}
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(true, MidiTurboFallbackReason::FirstLinkTestTimedOut);
			break;
		case Phase::WaitTest2:
			if(takeResponse(0x17, response))
				beginPayload();
			else if(m_phaseCycles > responseTimeoutCycles)
				fallBack(true, MidiTurboFallbackReason::SecondLinkTestTimedOut);
			break;
		case Phase::WaitFallbackReset:
			if(m_phaseCycles >= fallbackResetCycles)
				beginPayload();
			break;
		default:
			break;
		}

		m_baudAccumulator += static_cast<uint64_t>(_cycles) * m_bytesPerSecond;
		pumpWire(_midiPort);

		const auto enterWait = [this](const Phase _send, const Phase _wait)
		{
			if(m_phase == _send && m_wire.empty())
			{
				m_phase = _wait;
				m_phaseCycles = 0;
			}
		};
		enterWait(Phase::SendRequest, Phase::WaitAnswer);
		enterWait(Phase::SendNegotiation, Phase::WaitAck);
		enterWait(Phase::SendTest1, Phase::WaitTest1);
		enterWait(Phase::SendTest2, Phase::WaitTest2);

		if(m_phase == Phase::Payload && m_payloadCursor >= m_payload.size())
		{
			m_speedCode = 1;
			m_bytesPerSecond = 3125;
			m_baudAccumulator = 0;
			m_phase = Phase::DrainPayload;
		}
		if(m_phase == Phase::DrainPayload && _midiPort.queuedMidiByteCount() == 0)
		{
			m_phase = Phase::Idle;
			m_state.store(MidiSysexTransferState::Complete,
				std::memory_order_release);
		}
		if(m_phase == Phase::DrainCancellation && m_wire.empty()
			&& _midiPort.queuedMidiByteCount() == 0)
		{
			m_phase = Phase::Idle;
			m_state.store(MidiSysexTransferState::Cancelled,
				std::memory_order_release);
		}
		publishProgress();
	}
}
