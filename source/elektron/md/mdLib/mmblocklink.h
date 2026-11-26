#pragma once

#include <array>
#include <cstdint>

namespace md
{
	/*	Monomachine block link: the inter-DSP serial link of the Monomachine, carried as block transfers.

		Monomachine OS 1.32b runs in blocks of 16 sample frames (36864 DSP cycles). Both DSPs render three voices
		per block. The link goes one way, from DSP2 (tracks 1-3, ESSI0 transmitter and clock master) to DSP1
		(tracks 4-6, the codec, ESSI0 receiver), one 24 bit word per 96 cycles, on-demand mode. Per block

		- DSP1 toggles Port C bit 1 after its second voice and arms DMA 4 to receive 128 words from RX0 into the
		  four bus buffers of its next block (4 lines of 32 words, DOR2). The DMA 4 interrupt arms the channel
		  again for 48 words into a fifth buffer.
		- DSP2, after its third voice, waits for that toggle and arms DMA 1 to send the four bus buffers of the
		  block it has just rendered (4 lines of 32 words, DOR2) to TX00. The DMA 1 interrupt arms the channel
		  again for 48 words from a fifth buffer. It then renders its next block into its other set of buffers.

		That is everything the two programs observe of the link: the pin and its DMA 1 interrupt on DSP2, the
		DMA 4 interrupt and the received buffers on DSP1. Neither reads a DMA address or counter of the link,
		the DMA status register outside the interrupt vector, or the ESSI0 status register; DSP1 reads RX0 once
		per block and discards it. Both take their block timing from the codec DMA pointers.

		Clocking ESSI0 for this costs a peripheral pass on each DSP per 96-cycle slot, used or not, and a
		catch-up of DSP1 per word. The block link never clocks ESSI0 and moves each burst in one step, through
		the same DMA channels:

		- DSP2 arming a channel that feeds TX00 copies the words it is going to send. At the cycle at which the
		  last of them would have been fetched, the channel is run to its end (Dma::execRequests), which raises
		  its interrupt there.
		- DSP1 has the words in its buffer from the cycle at which the first of them would have arrived, and
		  DMA 4 ends its transfer and raises its interrupt at the cycle at which the last would have. A word
		  is never later than on the wire and can be up to a burst earlier. That side was chosen as the safe
		  one, it was not needed by any render or test so far: a word that is late is one DSP1 may read before
		  it is there if a send ends close to the start of DSP1's next block, while the buffers of the block
		  that is being received are not in use before that block (the codec played them a block ago; read
		  from the buffer rotation in the disassembly, not traced).
		- the pin carries the time of its toggle, like the Machinedrum's. It holds one toggle on its way to DSP2:
		  when DSP1 toggles again before DSP2 has reached the previous toggle, DSP2 sees that one early, and with
		  DSP1 two blocks or more ahead, toggles cancel out (pinOverwritten counts them). On OS 1.32b that
		  happens only in the first blocks after the link engages, while DSP1 is up to about eleven blocks ahead
		  (maxLateCycles 395,427 cycles), never later in the renders and tests so far. A queue that kept every toggle (as the timed block sync of the
		  serial path does) was tried: DSP2 then works through every block it is behind instead of skipping to
		  DSP1's, and one quiet machine (id 13) came out 22 dB lower than on the serial path, which the single
		  toggle matches.
		- a scheduler slice of a DSP that has a host command to serve is kept to one slot, see schedStep.
		  Without the link's words the DSPs advance a whole quantum at a time, and a DSP that takes the peek
		  command at the start of one waits inside its interrupt, where peripherals do not run, until the
		  ColdFire gets its turn: codec words came late by up to 1.3 frames, more than twice as often as on
		  the serial path. With the short slice none is late.

		When a word leaves, on the serial path (traced on OS 1.32b, see mmBlockLinkProducerDma): a channel armed
		while the transmit register is empty fetches its first word at once and one more on every slot from the
		first slot at or after the arm, each slot sending the word fetched before. A channel armed while the
		register still holds the last word of the previous send fetches its first word on the slot that sends
		that one. The first send of a block is of the first kind, the second, armed from the interrupt of the
		first, of the second kind.

		The DSPs then meet twice per block instead of 176 times, and neither serves 384 slots per block. Without
		the slots the next peripheral deadline of a DSP is the codec word, 1152 cycles away, so the wait loops
		that spinSkip accounts in one step (see mdblocklink.cpp) reach that far instead of to the next slot.

		What changes against the wire: the words of a burst reach DSP1's memory together when its first word
		would have arrived, not one by one.

		Engaged only for Monomachine OS 1.32b after the loaded programs match at the instructions this relies
		on, and only with GEARMULATOR_MM_BLOCK_LINK=1. The Machinedrum's block link (mdblocklink.h) is separate.
	*/
	struct MmBlockLink
	{
		static constexpr uint32_t SlotCycles = 96;			// ESSI0 word period in DSP cycles
		static constexpr uint32_t MaxBurstWords = 256;		// OS 1.32b sends 128 and 48
		static constexpr uint32_t MaxBursts = 4;			// on their way to DSP1 at one time, OS 1.32b has two

		// Fixed storage: bursts are made and taken on the audio thread
		struct Burst
		{
			uint64_t storeCycle = 0;						// DSP1 cycle at which the first word has arrived
			uint64_t deliverCycle = 0;						// DSP1 cycle at which the last word has arrived
			bool stored = false;							// the words are in DSP1's buffer, the transfer has not ended yet
			uint32_t count = 0;
			std::array<uint32_t, MaxBurstWords> words{};
		};

		bool enabled = false;				// wanted and applicable; cleared for good if the programs do not match
		bool active = false;

		std::array<uint64_t, 2> gridAnchor{};	// a slot of each DSP's ESSI0, on its own cycle counter. [0] DSP1, [1] DSP2

		// DSP2: the send that is under way
		bool sendPending = false;
		uint32_t sendChannel = 0;
		uint32_t sendWords = 0;
		uint32_t sendDcr = 0;				// the channel as armed for the send, run to its end only if still so
		uint32_t sendDsr = 0;
		uint32_t sendDco = 0;
		uint64_t lastSendSlot = 0;			// DSP2 cycle of the slot on which the last word of the newest send leaves

		// DSP1: bursts on their way, a ring with the oldest at arrivingFirst
		std::array<Burst, MaxBursts> arriving{};
		uint32_t arrivingFirst = 0;
		uint32_t arrivingCount = 0;

		// counters, GEARMULATOR_MM_BLOCK_LINK_STATS=1 prints them when the machine is destroyed
		uint64_t bursts = 0;
		uint64_t words = 0;
		uint64_t lateDeliveries = 0;		// DSP1 was already past the arrival time when DSP2 armed the send
		uint64_t maxLateCycles = 0;
		uint64_t receiverNotArmed = 0;		// a burst arrived while DMA 4 was not enabled
		uint64_t unsupportedPrograms = 0;	// DSP2 armed a transmit channel, or DSP1 its receiver, in a mode this
											// does not carry
		uint64_t cancelledSends = 0;		// DSP2 changed the channel of a send (DCR, DSR or DCO) before it was over
		uint64_t shortReceives = 0;			// DMA 4 took fewer words than the burst held (none counts too)
		uint64_t droppedBursts = 0;			// DSP1 had not taken the oldest burst when a fifth was sent
		uint64_t pinEdges = 0;				// block toggles DSP2 has seen
		uint64_t pinMaxDelay = 0;			// the most cycles between a toggle and the read that saw it
		uint64_t pinOverwritten = 0;		// toggles DSP2 does not see at their time: DSP1 toggled again before DSP2
											// reached the previous one, or several were on their way at engage
	};
}
