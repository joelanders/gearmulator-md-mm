#pragma once

#include "mdtypes.h"

#include <cstddef>
#include <cstdint>

namespace md
{
	// Scheduling and host-port limits used to interleave the ColdFire and two
	// DSPs on one host thread. These values preserve the current runtime
	// behavior; keeping them together makes model differences and units explicit.
	struct TransportPolicy
	{
		double backgroundQuantumMicroseconds;
		uint64_t catchUpMaxDspCycles;
		size_t hostReceiveIrqMinWords;
		size_t hostReceiveQueueCapacityWords;
		size_t hostTransmitBackpressureThresholdWords;
		uint64_t hostTransmitBackpressureReleaseUcCycles;
		bool exactEssiCycleDeadlines;
	};

	constexpr TransportPolicy transportPolicy(const MachineModel _model)
	{
		return _model == MachineModel::Monomachine
			? TransportPolicy{30.0, 100'000, 1, 16, 4, 200'000, true}
			: TransportPolicy{125.0, 100'000, 3, 16, 4, 200'000, true};
	}
}
