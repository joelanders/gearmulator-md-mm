#pragma once

#include "mdsysextransfer.h"

namespace md
{
	// Session identity, never serialized into projects. A dialog/command belongs
	// to one Device, one Hardware epoch, one restore generation, and one request.
	struct SysexImportTicket
	{
		uint64_t device = 0, hardware = 0, restore = 0, request = 0;
		bool operator==(const SysexImportTicket& other) const
		{
			return device == other.device && hardware == other.hardware
				&& restore == other.restore && request == other.request;
		}
		bool operator!=(const SysexImportTicket& other) const { return !(*this == other); }
	};
	enum class SysexImportStage : uint8_t
	{
		Idle, Preparing, Transferring, AwaitingReceiveMode,
		DeliveredUnverified, Cancelled, Failed, Invalidated
	};
	enum class SysexImportStartResult : uint8_t
	{
		Started, StaleRequest, WrongModel, Restoring, NotReady, Initializing,
		ConfirmationRequired, Busy
	};
	struct SysexImportProgress : MidiSysexTransferProgress
	{
		SysexImportTicket ticket;
		SysexImportStage stage = SysexImportStage::Idle;
		// No current protocol observation establishes durable firmware commitment.
		// Transport Complete must never be presented as a verified import.
		bool contentsVerified = false;
	};
}
