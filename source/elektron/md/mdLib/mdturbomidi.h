#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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
	// Machinedrum manual. All calls are externally serialized by Hardware's owning
	// synthLib::Plugin lock; the UART transmit observer runs synchronously on that
	// same scheduler path. No lock or allocation occurs while service() is running.
	class TurboMidiTransfer
	{
	public:
		explicit TurboMidiTransfer(uint64_t _clockHz);

		bool start(PreparedMidiSysexTransfer& _transfer,
			size_t _realtimeWriteBoundary);
		bool cancel(std::vector<uint8_t>& _retiredPayload);
		bool retirePayload(std::vector<uint8_t>& _retiredPayload);
		bool resumeReceiveMode(uint32_t _transferId, size_t _step);
		void service(uint32_t _cycles, bool _ingressDrained,
			MidiByteSink& _midiPort);
		void observeTransmitByte(uint8_t _byte);

		bool ownsMidiWire() const;
		size_t realtimeWriteBoundary() const { return m_realtimeWriteBoundary; }
		MidiSysexTransferProgress progress() const { return m_progress.read(); }
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
			SettleLink,
			WaitFallbackReset,
			Payload,
			DrainSds,
			WaitSds,
			DrainReceiveMode,
			WaitReceiveMode,
			DrainPayload,
			DrainCancellation
		};

		void queueMessage(uint8_t _command,
			std::initializer_list<uint8_t> _payload = {});
		void parseTransmitBytes();
		bool pushResponse(const Response& _message);
		bool takeResponse(uint8_t _command, Response& _message);
		void clearResponses();
		void beginPayload();
		void fallBack(bool _waitForPeerReset, MidiTurboFallbackReason _reason);
		void pumpWire(MidiByteSink& _midiPort);
		void publishProgress();
		void abortPayload(MidiSysexTransferError _error);
		void serviceSds();
		void retrySds();
		bool takeSdsResponse(Response& _message);

		class AccessGuard
		{
		public:
			explicit AccessGuard(std::atomic_flag& _flag)
				: m_flag(_flag)
				, m_acquired(!m_flag.test_and_set(std::memory_order_acquire))
			{
			}
			~AccessGuard()
			{
				if(m_acquired)
					m_flag.clear(std::memory_order_release);
			}
			explicit operator bool() const { return m_acquired; }

		private:
			std::atomic_flag& m_flag;
			bool m_acquired;
		};

		const uint64_t m_clockHz;
		std::vector<uint8_t> m_payload;
		std::vector<MidiSysexMessage> m_messages;
		size_t m_messageIndex = 0;
		bool m_sdsActive = false;
		bool m_sdsWaiting = false;
		uint8_t m_sdsPacket = 0;
		uint8_t m_packetRetries = 0;
		uint32_t m_retries = 0;
		uint32_t m_acknowledgedSamples = 0;
		uint32_t m_serviceSerial = 0;
		uint32_t m_transferId = 0;
		MachineModel m_model = MachineModel::Machinedrum;
		MidiSysexMessageKind m_receiveKind = MidiSysexMessageKind::UserDump;
		uint64_t m_sdsWaitCycles = 0;
		MidiSysexTransferError m_error = MidiSysexTransferError::None;
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
		bool m_captureTransmit = false;
		FixedByteQueue<4096> m_transmitBytes;
		std::atomic<uint64_t> m_overflow{0};
		uint64_t m_observedOverflow = 0;
		Response m_partialResponse;
		std::array<Response, 8> m_responses{};
		size_t m_responseRead = 0;
		size_t m_responseCount = 0;
		std::atomic<MidiSysexTransferState> m_state{MidiSysexTransferState::Idle};
		std::atomic_flag m_access = ATOMIC_FLAG_INIT;
		MidiSysexTransferProgressPublisher m_progress;
	};
}
