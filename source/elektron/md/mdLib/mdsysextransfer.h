#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "mdtypes.h"

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

	inline constexpr size_t g_midiSysexTransferMaxBytes = 8u * 1024u * 1024u;

	enum class MidiSysexStreamValidation : uint8_t
	{
		Valid,
		Empty,
		TooLarge,
		InvalidFraming,
		InvalidDataByte,
		ChecksumMismatch,
		UnsupportedMessage,
		WrongModel,
		FirmwareUpdate
	};

	// Accept only complete, concatenated Elektron user-data messages for this
	// machine. OS update messages are deliberately rejected by this user-data path.
	inline MidiSysexStreamValidation validateMidiSysexStream(
		const std::vector<uint8_t>& _bytes, const MachineModel _model)
	{
		if(_bytes.empty())
			return MidiSysexStreamValidation::Empty;
		if(_bytes.size() > g_midiSysexTransferMaxBytes)
			return MidiSysexStreamValidation::TooLarge;

		const uint8_t expectedProduct = _model == MachineModel::Monomachine
			? uint8_t{0x03} : uint8_t{0x02};
		size_t offset = 0;
		while(offset < _bytes.size())
		{
			if(offset + 8 > _bytes.size() || _bytes[offset] != 0xf0
				|| _bytes[offset + 1] != 0x00 || _bytes[offset + 2] != 0x20
				|| _bytes[offset + 3] != 0x3c)
				return MidiSysexStreamValidation::InvalidFraming;
			if(_bytes[offset + 4] != expectedProduct)
				return MidiSysexStreamValidation::WrongModel;

			const auto end = std::find(
				_bytes.begin() + static_cast<std::ptrdiff_t>(offset + 7),
				_bytes.end(), uint8_t{0xf7});
			if(end == _bytes.end())
				return MidiSysexStreamValidation::InvalidFraming;
			if(std::any_of(_bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1),
				end, [](const uint8_t _byte) { return _byte > 0x7f; }))
				return MidiSysexStreamValidation::InvalidDataByte;

			const uint8_t command = _bytes[offset + 6];
			if(command == 0x7e || command == 0x7f)
				return MidiSysexStreamValidation::FirmwareUpdate;
			const bool commonDataDump = command == 0x50 || command == 0x52
				|| command == 0x67 || command == 0x69;
			const bool digiProDump = _model == MachineModel::Monomachine
				&& command == 0x5d;
			if(!commonDataDump && !digiProDump)
				return MidiSysexStreamValidation::UnsupportedMessage;

			const size_t messageSize = static_cast<size_t>(end
				- (_bytes.begin() + static_cast<std::ptrdiff_t>(offset))) + 1;
			// Elektron data dumps carry [checksum MSB, checksum LSB, length MSB,
			// length LSB] immediately before EOX. Short request/status messages do
			// not. Every command admitted above is a data dump, so a short message
			// is incomplete rather than a valid control request.
			if(messageSize < 13)
				return MidiSysexStreamValidation::ChecksumMismatch;
			const size_t checksumPosition = offset + messageSize - 5;
			uint32_t sum = 0;
			// DigiPRO excludes its destination slot at byte 9. Other Elektron
			// data dumps include byte 9 in the common checksum range.
			const size_t checksumBegin = digiProDump ? offset + 10 : offset + 9;
			for(size_t i = checksumBegin; i < checksumPosition; ++i)
				sum += _bytes[i];
			const uint16_t checksum = static_cast<uint16_t>(
				(_bytes[checksumPosition] << 7)
				| _bytes[checksumPosition + 1]);
			const uint16_t length = static_cast<uint16_t>(
				(_bytes[checksumPosition + 2] << 7)
				| _bytes[checksumPosition + 3]);
			if((sum & 0x3fff) != checksum
				|| messageSize - 10 > 0x3fff
				|| length != messageSize - 10)
				return MidiSysexStreamValidation::ChecksumMismatch;
			offset = static_cast<size_t>(end - _bytes.begin()) + 1;
		}
		return MidiSysexStreamValidation::Valid;
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

	private:
		explicit PreparedMidiSysexTransfer(std::vector<uint8_t>&& _bytes)
			: m_bytes(std::move(_bytes))
		{
		}

		friend class TurboMidiTransfer;
		friend std::optional<PreparedMidiSysexTransfer>
			prepareMidiSysexTransfer(std::vector<uint8_t> _bytes);
		std::vector<uint8_t> m_bytes;
	};

	inline std::optional<PreparedMidiSysexTransfer> prepareMidiSysexTransfer(
		std::vector<uint8_t> _bytes)
	{
		if(_bytes.empty() || _bytes.front() != 0xf0 || _bytes.back() != 0xf7)
			return std::nullopt;
		return PreparedMidiSysexTransfer(std::move(_bytes));
	}

	enum class MidiSysexTransferState : uint8_t
	{
		Idle,
		Queued,
		NegotiatingTurbo,
		Sending,
		Complete,
		Cancelling,
		Cancelled
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
					m_fallbackCount.load(std::memory_order_relaxed)};
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
	};
}
