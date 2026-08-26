#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <vector>

#include "mdfixedbytequeue.h"
#include "mdsysextransfer.h"

namespace md
{
	class MidiByteSink
	{
	public:
		virtual ~MidiByteSink() = default;
		virtual bool tryWriteMidiByte(uint8_t _byte) = 0;
		virtual size_t queuedMidiByteCount() const = 0;
	};

	// Host side of the TurboMIDI protocol documented in Appendix C of the
	// Machinedrum user manual. This class knows only MIDI bytes and timing; it
	// neither inspects nor modifies the emulated instrument's memory.
	class TurboMidiTransfer
	{
	public:
		TurboMidiTransfer(uint64_t _clockHz,
			std::shared_ptr<MidiSysexTransferProgressPublisher> _publisher);

		bool start(PreparedMidiSysexTransfer& _transfer,
			size_t _realtimeWriteBoundary);
		void service(uint32_t _cycles, bool _ingressDrained,
			MidiByteSink& _midiPort);
		void observeTransmitByte(uint8_t _byte);

		bool ownsMidiWire() const;
		size_t realtimeWriteBoundary() const
		{
			return m_realtimeWriteBoundary;
		}
		MidiSysexTransferProgress progress() const
		{
			return m_publisher->read();
		}
		uint64_t overflowCount() const
		{
			return m_overflow.load(std::memory_order_relaxed);
		}

	private:
		struct Response
		{
			static constexpr size_t Capacity = 256;
			std::array<uint8_t, Capacity> bytes{};
			size_t size = 0;

			void clear() { size = 0; }
			bool empty() const { return size == 0; }
			uint8_t operator[](size_t _index) const { return bytes[_index]; }
		};

		enum class Phase : uint8_t
		{
			Idle,
			BeginNegotiation,
			SendRequest,
			WaitAnswer,
			SendNegotiation,
			WaitAck,
			SendTest1,
			WaitTest1,
			SendTest2,
			WaitTest2,
			WaitFallbackReset,
			Payload,
			DrainPayload
		};

		void publishProgress();
		void queueMessage(uint8_t _command,
			std::initializer_list<uint8_t> _payload = {});
		void parseTransmitBytes();
		bool pushResponse(const Response& _message);
		bool takeResponse(uint8_t _command, Response& _message);
		void clearResponses();
		void beginPayload();
		void fallBack(bool _waitForPeerReset, MidiTurboFallbackReason _reason);
		void pumpWire(MidiByteSink& _midiPort);

		const uint64_t m_clockHz;
		std::shared_ptr<MidiSysexTransferProgressPublisher> m_publisher;
		mutable std::mutex m_mutex;
		std::vector<uint8_t> m_payload;
		size_t m_payloadCursor = 0;
		size_t m_total = 0;
		size_t m_sent = 0;
		size_t m_realtimeWriteBoundary = 0;
		FixedByteQueue<4096> m_wire;
		uint64_t m_baudAccumulator = 0;
		Phase m_phase = Phase::Idle;
		uint8_t m_speed1 = 1;
		uint8_t m_speed2 = 1;
		uint8_t m_speedCode = 1;
		uint32_t m_bytesPerSecond = 3125;
		uint64_t m_phaseCycles = 0;
		uint64_t m_activeSenseCycles = 0;
		MidiTurboFallbackReason m_fallbackReason = MidiTurboFallbackReason::None;
		uint32_t m_fallbackCount = 0;
		std::atomic<bool> m_captureTransmit{false};
		mutable std::mutex m_transmitMutex;
		FixedByteQueue<4096> m_transmitBytes;
		std::atomic<uint64_t> m_overflow{0};
		Response m_partialResponse;
		std::array<Response, 8> m_responses{};
		size_t m_responseRead = 0;
		size_t m_responseCount = 0;
		std::atomic<MidiSysexTransferState> m_state{MidiSysexTransferState::Idle};
	};
}
