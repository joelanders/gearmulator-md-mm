#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "mdtypes.h"
#include "mdsysexfile.h"

namespace md
{
	class TurboMidiTransfer;

	inline const char* midiTurboSpeedLabel(const uint8_t _code)
	{
		switch(_code)
		{
		case 2: return "2";
		case 3: return "3.33";
		case 4: return "4";
		case 5: return "5";
		case 6: return "6.66";
		case 7: return "8";
		case 8: return "10";
		default: return "1";
		}
	}


	class PreparedMidiSysexTransfer
	{
	public:
		PreparedMidiSysexTransfer(PreparedMidiSysexTransfer&&) noexcept = default;
		PreparedMidiSysexTransfer& operator=(PreparedMidiSysexTransfer&&) noexcept = default;
		PreparedMidiSysexTransfer(const PreparedMidiSysexTransfer&) = delete;
		PreparedMidiSysexTransfer& operator=(const PreparedMidiSysexTransfer&) = delete;

		bool empty() const { return m_bytes.empty(); }
		size_t size() const { return m_bytes.size(); }
		MachineModel model() const { return m_model; }
		MidiSysexMessageKind firstKind() const { return m_messages.front().kind; }
		bool contains(MidiSysexMessageKind _kind) const
		{
			return std::any_of(m_messages.begin(), m_messages.end(),
				[_kind](const auto& message) { return message.kind == _kind; });
		}

	private:
		explicit PreparedMidiSysexTransfer(std::vector<uint8_t>&& _bytes,
			std::vector<MidiSysexMessage>&& _messages, MachineModel _model)
			: m_bytes(std::move(_bytes))
			, m_messages(std::move(_messages)), m_model(_model)
		{
		}

		friend class TurboMidiTransfer;
		friend std::optional<PreparedMidiSysexTransfer>
			prepareMidiSysexTransfer(std::vector<uint8_t> _bytes, MachineModel _model,
				MidiSysexStreamValidation* _validation);
		std::vector<uint8_t> m_bytes;
		std::vector<MidiSysexMessage> m_messages;
		MachineModel m_model;
	};

	inline std::optional<PreparedMidiSysexTransfer> prepareMidiSysexTransfer(
		std::vector<uint8_t> _bytes, MachineModel _model = MachineModel::Machinedrum,
		MidiSysexStreamValidation* _validation = nullptr)
	{
		std::vector<MidiSysexMessage> messages;
		const auto result = parseMidiSysexFile(_bytes, _model, &messages);
		if(_validation) *_validation = result;
		if(result != MidiSysexStreamValidation::Valid)
			return std::nullopt;
		// The MD receiver is always device 0. Retarget valid archived SDS files,
		// adjusting the packet XOR rather than rejecting another sender's device ID.
		for(const auto& message : messages)
		{
			if(message.kind == MidiSysexMessageKind::SdsPacket)
				_bytes[message.offset + 125] ^= _bytes[message.offset + 2];
			if(message.kind == MidiSysexMessageKind::SdsHeader
				|| message.kind == MidiSysexMessageKind::SdsPacket)
				_bytes[message.offset + 2] = 0;
		}
		return PreparedMidiSysexTransfer(std::move(_bytes), std::move(messages), _model);
	}

	enum class MidiSysexTransferState : uint8_t
	{
		Idle,
		Queued,
		NegotiatingTurbo,
		Sending,
		WaitingForDevice,
		WaitingForReceiveMode,
		Retrying,
		Complete,
		Cancelling,
		Cancelled,
		Failed
	};

	enum class MidiSysexTransferError : uint8_t
	{
		None, DeviceCancelled, ReplyTimedOut, RetryLimit, ResponseOverflow
	};

	enum class MidiTurboFallbackReason : uint8_t
	{
		None,
		MalformedSpeedAnswer,
		NoCommonCertifiedSpeed,
		CapabilityRequestTimedOut,
		SpeedAcknowledgementTimedOut,
		FirstLinkTestBadData,
		FirstLinkTestTimedOut,
		SecondLinkTestTimedOut
	};

	struct MidiSysexTransferProgress
	{
		MidiSysexTransferState state = MidiSysexTransferState::Idle;
		size_t sent = 0;
		size_t total = 0;
		uint8_t speedCode = 1;
		bool turbo = false;
		MidiTurboFallbackReason fallbackReason = MidiTurboFallbackReason::None;
		uint32_t fallbackCount = 0;
		MidiSysexTransferError error = MidiSysexTransferError::None;
		uint32_t retries = 0;
		uint32_t acknowledgedSamples = 0;
		uint32_t serviceSerial = 0;
		uint32_t transferId = 0;
		size_t receiveStep = 0;
		MidiSysexMessageKind receiveKind = MidiSysexMessageKind::UserDump;
	};

	// Coherent lock-free observations for UI and diagnostic readers. Transport
	// mutation is single-owner, while this publisher makes accidental unlocked
	// progress reads safe without putting a mutex on the audio path.
	class MidiSysexTransferProgressPublisher
	{
	public:
		static_assert(std::atomic<uint32_t>::is_always_lock_free);
		static_assert(std::atomic<size_t>::is_always_lock_free);
		static_assert(std::atomic<uint8_t>::is_always_lock_free);
		static_assert(std::atomic<bool>::is_always_lock_free);
		static_assert(std::atomic<MidiSysexTransferState>::is_always_lock_free);
		static_assert(std::atomic<MidiTurboFallbackReason>::is_always_lock_free);
		static_assert(std::atomic<MidiSysexTransferError>::is_always_lock_free);
		static_assert(std::atomic<MidiSysexMessageKind>::is_always_lock_free);

		void publish(const MidiSysexTransferProgress& _progress)
		{
			m_sequence.fetch_add(1, std::memory_order_acq_rel);
			m_state.store(_progress.state, std::memory_order_relaxed);
			m_sent.store(_progress.sent, std::memory_order_relaxed);
			m_total.store(_progress.total, std::memory_order_relaxed);
			m_speedCode.store(_progress.speedCode, std::memory_order_relaxed);
			m_turbo.store(_progress.turbo, std::memory_order_relaxed);
			m_fallbackReason.store(_progress.fallbackReason, std::memory_order_relaxed);
			m_fallbackCount.store(_progress.fallbackCount, std::memory_order_relaxed);
			m_error.store(_progress.error, std::memory_order_relaxed);
			m_retries.store(_progress.retries, std::memory_order_relaxed);
			m_acknowledgedSamples.store(_progress.acknowledgedSamples, std::memory_order_relaxed);
			m_serviceSerial.store(_progress.serviceSerial, std::memory_order_relaxed);
			m_transferId.store(_progress.transferId, std::memory_order_relaxed);
			m_receiveStep.store(_progress.receiveStep, std::memory_order_relaxed);
			m_receiveKind.store(_progress.receiveKind, std::memory_order_relaxed);
			m_sequence.fetch_add(1, std::memory_order_release);
		}

		MidiSysexTransferProgress read() const
		{
			for(;;)
			{
				const auto before = m_sequence.load(std::memory_order_acquire);
				if((before & 1u) != 0)
					continue;
				const MidiSysexTransferProgress result{
					m_state.load(std::memory_order_relaxed),
					m_sent.load(std::memory_order_relaxed),
					m_total.load(std::memory_order_relaxed),
					m_speedCode.load(std::memory_order_relaxed),
					m_turbo.load(std::memory_order_relaxed),
					m_fallbackReason.load(std::memory_order_relaxed),
					m_fallbackCount.load(std::memory_order_relaxed),
					m_error.load(std::memory_order_relaxed),
					m_retries.load(std::memory_order_relaxed),
					m_acknowledgedSamples.load(std::memory_order_relaxed),
					m_serviceSerial.load(std::memory_order_relaxed),
					m_transferId.load(std::memory_order_relaxed),
					m_receiveStep.load(std::memory_order_relaxed),
					m_receiveKind.load(std::memory_order_relaxed)};
				if(before == m_sequence.load(std::memory_order_acquire))
					return result;
			}
		}

	private:
		std::atomic<uint32_t> m_sequence{0};
		std::atomic<MidiSysexTransferState> m_state{MidiSysexTransferState::Idle};
		std::atomic<size_t> m_sent{0};
		std::atomic<size_t> m_total{0};
		std::atomic<uint8_t> m_speedCode{1};
		std::atomic<bool> m_turbo{false};
		std::atomic<MidiTurboFallbackReason> m_fallbackReason{
			MidiTurboFallbackReason::None};
		std::atomic<uint32_t> m_fallbackCount{0};
		std::atomic<MidiSysexTransferError> m_error{MidiSysexTransferError::None};
		std::atomic<uint32_t> m_retries{0}, m_acknowledgedSamples{0}, m_serviceSerial{0};
		std::atomic<uint32_t> m_transferId{0};
		std::atomic<size_t> m_receiveStep{0};
		std::atomic<MidiSysexMessageKind> m_receiveKind{MidiSysexMessageKind::UserDump};
	};
}
