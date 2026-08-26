#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace md
{
	class Hardware;
	class TurboMidiTransfer;

	// Owns one validated, fully framed SysEx stream before it crosses into the
	// emulation-owned transfer state. Moving file-sized storage into this object is
	// intentionally separate from the short transfer-mutex commit.
	class PreparedMidiSysexTransfer
	{
	public:
		PreparedMidiSysexTransfer(PreparedMidiSysexTransfer&&) noexcept = default;
		PreparedMidiSysexTransfer& operator=(PreparedMidiSysexTransfer&&) noexcept = default;
		PreparedMidiSysexTransfer(const PreparedMidiSysexTransfer&) = delete;
		PreparedMidiSysexTransfer& operator=(const PreparedMidiSysexTransfer&) = delete;

		bool empty() const { return m_bytes.empty(); }
		size_t size() const { return m_bytes.size(); }
		const uint8_t* data() const { return m_bytes.data(); }

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
		Complete
	};

	// Stable, format-free explanation for a TurboMIDI negotiation that continued
	// at standard MIDI speed. This is published with transfer progress so UI and
	// test code can report it away from the emulation/audio thread.
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

	// A single transfer owner publishes complete observations while any number of
	// UI/status readers take coherent copies without entering the transfer mutex.
	// The payload fields are atomic too, avoiding the data race caused by a
	// sequence counter around ordinary memory.
	class MidiSysexTransferProgressPublisher
	{
	public:
		static_assert(std::atomic<uint32_t>::is_always_lock_free,
			"transfer publication sequence must be lock-free");
		static_assert(std::atomic<MidiSysexTransferState>::is_always_lock_free,
			"transfer publication state must be lock-free");
		static_assert(std::atomic<size_t>::is_always_lock_free,
			"transfer publication counters must be lock-free");
		static_assert(std::atomic<uint8_t>::is_always_lock_free,
			"transfer publication speed must be lock-free");
		static_assert(std::atomic<bool>::is_always_lock_free,
			"transfer publication flags must be lock-free");
		static_assert(std::atomic<MidiTurboFallbackReason>::is_always_lock_free,
			"transfer fallback status must be lock-free");

		void publish(const MidiSysexTransferProgress& _progress)
		{
			// Hardware serializes start and service publication with its transfer
			// mutex, so this sequence always has exactly one writer at a time.
			m_sequence.fetch_add(1);
			m_state.store(_progress.state);
			m_sent.store(_progress.sent);
			m_total.store(_progress.total);
			m_speedCode.store(_progress.speedCode);
			m_turbo.store(_progress.turbo);
			m_fallbackReason.store(_progress.fallbackReason);
			m_fallbackCount.store(_progress.fallbackCount);
			m_sequence.fetch_add(1);
		}

		MidiSysexTransferProgress read() const
		{
			for(;;)
			{
				const auto before = m_sequence.load();
				if((before & 1u) != 0)
					continue;

				const MidiSysexTransferProgress result{
					m_state.load(),
					m_sent.load(),
					m_total.load(),
					m_speedCode.load(),
					m_turbo.load(),
					m_fallbackReason.load(),
					m_fallbackCount.load()
				};
				const auto after = m_sequence.load();
				if(before == after)
					return result;
			}
		}

		void reset()
		{
			publish({});
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
