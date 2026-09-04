#include "synthLib/realtimeInstrumentation.h"

#include <cstdlib>
#include <iostream>

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "synthLibRealtimeInstrumentationTest: " << _message << '\n';
		std::exit(1);
	}
}

int main()
{
	synthLib::RealtimeInstrumentation instrumentation;
	instrumentation.setEnabled(false);
	instrumentation.recordSynthProcess(100);
	instrumentation.recordSynthProcessLockWait(20);
	instrumentation.recordResampler(70, 60, 64, true);
	{
		synthLib::RealtimeInstrumentation::CallbackScope callback(
			instrumentation, 64, 48'000.0);
		synthLib::RealtimeInstrumentation::recordCurrentCallbackJitCompilation();
		synthLib::RealtimeInstrumentation::DeferredCandidateScope candidate(64);
	}
	require(instrumentation.snapshot().synthProcessCount == 0,
		"disabled instrumentation recorded work");
	require(instrumentation.snapshot().outerHostCallbackCount == 0
		&& instrumentation.snapshot().jitCompilationCount == 0
		&& instrumentation.snapshot().deferredCandidateAdvanceCount == 0,
		"disabled callback scopes recorded work");

	instrumentation.reset();
	instrumentation.setEnabled(true);
	synthLib::RealtimeInstrumentation::recordCurrentCallbackJitCompilation();
	require(instrumentation.snapshot().jitCompilationCount == 0,
		"JIT work outside an outer host callback was attributed");
	{
		synthLib::RealtimeInstrumentation::CallbackScope callback(
			instrumentation, 1, 1'000'000'000.0);
		callback.setActiveOutputLayout(3, 6);
		synthLib::RealtimeInstrumentation::recordCurrentCallbackJitCompilation();
		{
			synthLib::RealtimeInstrumentation::DeferredCandidateScope candidate(64);
			synthLib::RealtimeInstrumentation::recordCurrentCallbackJitCompilation();
		}
	}
	{
		synthLib::RealtimeInstrumentation::CallbackScope callback(
			instrumentation, 64, 48'000.0, true);
	}
	instrumentation.recordSynthProcess(100);
	instrumentation.recordSynthProcess(250);
	instrumentation.recordSynthProcessLockWait(20);
	instrumentation.recordSynthProcessLockWait(80);
	instrumentation.recordResampler(70, 60, 64, true);

	const auto snapshot = instrumentation.snapshot();
	require(snapshot.enabled && snapshot.outerHostCallbackCount == 2,
		"enabled callback was not counted");
	require(snapshot.outerHostCallbackOverrunCount >= 1
		&& snapshot.outerHostCallbackMaxOverrunNanoseconds > 0,
		"callback budget overrun was not counted");
	require(snapshot.jitCompilationCount == 2
		&& snapshot.liveJitCompilationCount == 1
		&& snapshot.deferredCandidateJitCompilationCount == 1
		&& snapshot.callbacksWithJitCompilation == 1,
		"callback JIT attribution is wrong");
	require(snapshot.deferredDualMachineCallbackCount == 1
		&& snapshot.deferredCandidateAdvanceCount == 1
		&& snapshot.deferredCandidateFrames == 64
		&& snapshot.deferredCandidateNanoseconds > 0,
		"deferred-candidate work was not attributed");
	require(snapshot.callbacksWithAuxOutputBuses == 1
		&& snapshot.maximumActiveOutputBuses == 3
		&& snapshot.maximumActiveOutputChannels == 6,
		"active output layout was not recorded");
	require(snapshot.bypassedCallbackCount == 1,
		"bypassed outer callback was not distinguished");
	require(snapshot.synthProcessCount == 2
		&& snapshot.synthProcessNanoseconds == 350
		&& snapshot.synthProcessMaxNanoseconds == 250,
		"plugin process totals are wrong");
	require(snapshot.synthProcessLockWaitNanoseconds == 100
		&& snapshot.synthProcessLockWaitMaxNanoseconds == 80,
		"lock-wait totals are wrong");
	require(snapshot.resamplerCallCount == 1
		&& snapshot.resamplerNanoseconds == 70
		&& snapshot.deviceProcessNanoseconds == 60
		&& snapshot.resamplerHostFrames == 64
		&& snapshot.resamplingActiveCallbackCount == 1,
		"resampler/device work totals are wrong");

	instrumentation.reset();
	require(instrumentation.snapshot().outerHostCallbackCount == 0,
		"reset did not clear counters");
	std::cout << "synthLib realtime instrumentation: PASS\n";
	return 0;
}
