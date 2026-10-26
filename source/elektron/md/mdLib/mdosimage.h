#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mdtypes.h"

namespace md
{
	// Offline decoder for Elektron OS upgrade SysEx files (Machinedrum and
	// Monomachine). Lets an OS image be loaded directly from a .syx file with
	// no MIDI flashing ceremony: the firmware cannot distinguish flash it
	// programmed itself from identical bytes placed at boot.
	//
	// Wire format, derived black-box by correlating upgrade files against
	// known flash images (never from disassembly):
	//   File   = N data messages + end marker + empty message + verify command.
	//   Data   = F0 00 20 3C PP 00 7E | cs1 cs2 00 a3 a2 a1 a0 00
	//            | 32 x (collector,data,datum) | F7,
	//     PP      = product id (0x02 Machinedrum, 0x03 Monomachine).
	//     a3..a0  = flash byte address / 16 as four nibbles. The first data
	//               message addresses 0x4000 (bootloader is never included).
	//     cs1,cs2 = integrity field, nibble-split (not needed for decoding).
	//     each (c,a,b) group = one big-endian 16-bit word
	//               w = datum | (data << 7) | (collector << 14),
	//               i.e. 64 flash bytes per message.
	// Messages must start at 0x4000 (past the bootloader) and stay strictly
	// contiguous; a forward gap fails loudly, while a message overlapping
	// already-placed bytes counts as framing. Short trailing data blocks
	// (fewer than 32 groups, e.g. a final partial block) are accepted as
	// long as they continue the run.
	// Contains no vendor material: pure bit-layout arithmetic.
	struct OsUpgradeMessage
	{
		uint32_t address = 0;			// flash byte address of data[0]
		std::vector<uint8_t> data;		// decoded bytes (multiple of 2)
	};

	struct OsUpgradeDecodeResult
	{
		MachineModel model = MachineModel::Machinedrum;
		std::vector<OsUpgradeMessage> messages;
		uint32_t baseAddress = 0;		// address of messages.front()
		uint32_t endAddress = 0;		// one past messages.back() end
		uint32_t dataMessageCount = 0;
		uint32_t framingMessageCount = 0;	// end marker + empty + verify
		bool hasVerifyCommand = false;
	};

	// Decodes a whole .syx file. Returns false (result untouched) on any
	// structural problem: truncated message, unknown header/command, mixed
	// product ids, non-nibble address, out-of-range datum, or a gap/overlap
	// in message addresses.
	bool decodeOsUpgradeSysex(OsUpgradeDecodeResult& _result,
		const uint8_t* _data, size_t _size);
	inline bool decodeOsUpgradeSysex(OsUpgradeDecodeResult& _result,
		const std::vector<uint8_t>& _data)
	{
		return decodeOsUpgradeSysex(_result, _data.data(), _data.size());
	}
}
