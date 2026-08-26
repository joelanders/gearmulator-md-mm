#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "mdtypes.h"

namespace md
{
	// Logical panel controls shared by the MD and MM editors. The physical packet
	// is intentionally resolved only after the device model is known.
	enum class PanelControl : uint8_t
	{
		Trigger1,
		Trigger2,
		Trigger3,
		Trigger4,
		Trigger5,
		Trigger6,
		Trigger7,
		Trigger8,
		Trigger9,
		Trigger10,
		Trigger11,
		Trigger12,
		Trigger13,
		Trigger14,
		Trigger15,
		Trigger16,
		Track1,
		Track2,
		Track3,
		Track4,
		Track5,
		Track6,
		Tempo,
		SynthesisEffectsRouting,
		Function,
		Kit,
		Enter,
		Exit,
		Up,
		Down,
		Left,
		Right,
		BankGroup,
		BankA,
		BankB,
		BankC,
		BankD,
		Record,
		Play,
		Stop,
		DataPageForward,
		DataPageBackward,
		Scale,
		PatternSong,
		TrigSelect,
		SongEnable,
		ClassicExtended,
	};

	// Rotaries send signed deltas rather than scan-row masks, so they are kept
	// separate from PanelControl/PanelPacket.
	enum class PanelEncoder : uint8_t
	{
		DataEntryA,
		DataEntryB,
		DataEntryC,
		DataEntryD,
		DataEntryE,
		DataEntryF,
		DataEntryG,
		DataEntryH,
		Level,
		SoundSelection,
	};

	struct PanelPacket
	{
		uint8_t row = 0;
		uint8_t mask = 0;

		constexpr bool operator==(const PanelPacket& _other) const
		{
			return row == _other.row && mask == _other.mask;
		}
	};

	// Returns no packet when a logical control is not yet verified for that
	// model. Callers must leave it inert rather than substitute another model's
	// packet.
	std::optional<PanelPacket> panelPacket(MachineModel _model,
		PanelControl _control);

	std::optional<uint8_t> panelEncoderCommand(MachineModel _model,
		PanelEncoder _encoder);

	// Button packets are bit masks within a panel scan row. This state holder
	// merges simultaneous presses and removes only the released control's bit.
	class PanelRowState
	{
	public:
		PanelPacket press(const PanelPacket& _packet);
		PanelPacket release(const PanelPacket& _packet);
		uint8_t mask(uint8_t _row) const;
		void reset();

	private:
		static constexpr uint8_t g_firstRow = 0x20;
		static constexpr uint8_t g_lastRow = 0x25;
		static constexpr size_t g_rowCount = g_lastRow - g_firstRow + 1;

		std::array<uint8_t, g_rowCount> m_masks{};
	};

	// Cross-thread ingress for complete UART2 panel packets. This is a bounded MPSC
	// ring: any thread may tryPush(), while exactly one emulation thread drains it.
	// It neither allocates nor waits. A full queue rejects the complete packet and
	// increments overflowCount(); callers that cannot lose an event must retry it.
	class PanelInputQueue
	{
	public:
		static constexpr size_t g_capacityPackets = 512;
		static constexpr size_t g_maxDrainPackets = 128;
		using DrainBuffer = std::array<PanelPacket, g_maxDrainPackets>;

		PanelInputQueue();
		bool tryPush(uint8_t _command, uint8_t _argument);
		bool push(uint8_t _command, uint8_t _argument)
		{
			return tryPush(_command, _argument);
		}
		// Advisory counted-work gate for the emulation thread. The producer publishes
		// the count before the slot sequence, so true may briefly precede a drainable
		// packet; drain()'s sequence acquire remains the packet-publication authority.
		// False never consumes a wake: the count stays nonzero until the consumer drains.
		bool hasPending() const
		{
			return m_pendingPackets.load(std::memory_order_acquire) != 0;
		}
		// Retained byte-count convention for Hardware::getPendingPanelInputBytes().
		size_t size() const;
		size_t pendingPackets() const;
		size_t overflowCount() const;
		size_t drain(DrainBuffer& _destination,
			size_t _maximumPackets = g_maxDrainPackets);

	private:
		friend struct PanelInputQueueTestAccess;
		struct Slot
		{
			std::atomic<size_t> sequence{0};
			PanelPacket packet{};
		};

		static_assert(std::atomic<size_t>::is_always_lock_free,
			"PanelInputQueue requires lock-free size_t atomics");
		std::array<Slot, g_capacityPackets> m_slots{};
		std::atomic<size_t> m_enqueuePosition{0};
		size_t m_dequeuePosition = 0; // single-consumer-owned
		std::atomic<size_t> m_pendingPackets{0};
		std::atomic<size_t> m_overflowCount{0};
	};
}
