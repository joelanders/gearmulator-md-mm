#pragma once

#include <array>
#include <cstdint>

namespace md
{
	/*	Block link: the Machinedrum's inter-DSP serial link, carried as block transfers.

		Machinedrum OS 1.63 runs in blocks of 32 sample frames. Per block

		- the producer (DSP2) renders each of its 16 tracks into 32 words and sends them through DMA 0 and
		  ESSI0, one track after the other. Before a send it waits until DMA 0 has finished the previous one.
		- the mixer (DSP1) receives all 512 words through DMA 4 into Y:$600 and reads DDR4 to see how far the
		  transfer has come before it mixes a track.
		- once per block the mixer hands 64 words to DMA 2 and ESSI0 for the producer, toggles Port C bit 1 and
		  arms DMA 4 for the next 512 words. The producer waits for that toggle before it sends track 1, and arms
		  its own DMA 2 for the 64 words when it has seen it.

		That is everything the two programs observe of the link: DDR4 on the mixer, the enable bit of DCR0 and
		the Port C pin on the producer, and the buffers. Neither reads the ESSI0 or DMA status registers for it
		and neither uses a link interrupt.

		Clocking ESSI0 for this costs two peripheral passes per 96-cycle word slot and keeps both DSPs within a
		slot of each other, about two million scheduler events per second of audio. The block link does the
		transfers at the DMA registers instead and never clocks ESSI0:

		- the producer arming DMA 0 copies the 32 words to the mixer. They count as arrived one per slot, on the
		  slot grids the two ports were running on, so DDR4 reads what it would have read.
		- the mixer arming DMA 2 takes the 64 words, the producer arming DMA 2 receives them.
		- the pin carries the time of its toggle, so the producer sees it at the machine time the mixer drove
		  it, whichever of the two the scheduler ran first.

		The two DSPs then meet about twice per block. A read whose answer depends on what the other DSP has not
		executed yet catches that DSP up first, so nothing is ever answered from stale state.

		Engaged only for Machinedrum OS 1.63 and only after the loaded programs match at the addresses this
		relies on. GEARMULATOR_MDMM_BLOCK_LINK=0 keeps the serial path.
	*/
	struct BlockLink
	{
		static constexpr uint32_t SlotCycles = 96;			// ESSI0 word period in DSP cycles
		static constexpr uint32_t TrackWords = 32;
		static constexpr uint32_t TrackCount = 16;
		static constexpr uint32_t ReverseWords = 64;

		/*	The first word of a send leaves on the producer's slot at or after (arm time + TransmitOffset). Found
			by tracing the serial path with one instruction per block: with -96 every DDR4 read of 110 blocks
			falls on the same cycle on both paths. The DMA request is already pending when the channel is armed,
			so the first word is fetched for the slot that is under way.
		*/
		static constexpr int32_t TransmitOffset = -96;

		/*	Background quantum while engaged, in microseconds. The serial link kept the DSPs within a slot of each
			other, which also bounded how far they ran ahead of the ColdFire. Without it the Machinedrum's 125 us
			lets a DSP answer the ColdFire up to 5.5 frames in its future, the ColdFire busy-waits for that long
			and its update loop slows down by a tenth (trigger to sound 5.5 ms instead of 5.1 ms). 30 us, the
			Monomachine's quantum, gives 4.95 ms and costs less host CPU than 125 us because both sides wait less.
		*/
		static constexpr double QuantumMicroseconds = 30.0;

		bool enabled = false;				// wanted and applicable; cleared for good if the programs do not match
		bool active = false;				// engaged: ESSI0 is no longer clocked on either DSP
		bool clockOffPending = false;		// the fine slot schedule is removed at the next scheduler step

		std::array<uint64_t, 2> base{};		// machine cycle of each DSP's rate-lock origin. [0] mixer, [1] producer
		std::array<uint32_t, 2> gridPhase{};	// ESSI0 slot grid of each DSP, machine cycles modulo SlotCycles
		double quantumFrames = 0.0;

		// producer -> mixer: the sends of the current block
		std::array<uint64_t, TrackCount> firstArrival{};	// machine cycle at which the first word of a send arrives
		uint32_t sendCount = 0;
		uint64_t sendDone = 0;				// machine cycle at which DMA 0 has finished the newest send
		// A send that the producer arms before the mixer has armed DMA 4 for the block waits here
		bool windowArmed = false;
		uint32_t windowBase = 0;			// DDR4 as the mixer armed it
		std::array<std::array<uint32_t, TrackWords>, TrackCount> held{};
		uint32_t heldFirst = 0;				// sends from this index on are still held

		// mixer -> producer: the block toggle and the 64 words
		uint32_t levelBefore = 0;
		uint32_t level = 0;
		uint64_t toggleTime = 0;
		uint32_t pinLastRead = ~0u;			// what the producer's last read of the pin returned
		bool producerWaiting = false;		// the producer polls the pin and has seen every toggle so far
		std::array<uint32_t, ReverseWords> reverse{};
		bool reverseValid = false;
		bool reverseArmed = false;			// the producer armed its receive before the mixer handed the words over
		uint32_t reverseDestination = 0;
	};
}
