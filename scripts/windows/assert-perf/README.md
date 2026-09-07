# Windows assertion validation and synthetic core timing

This diagnostic branch reuses the existing `elektron-windows.yml` workflow.
Its `build_mdmm.ps1` hooks build and run this suite on the same hosted runner
as the ordinary Windows VST3/Standalone integration checks, before the expensive
product build so harness failures surface early. Debug uses `/Z7` embedded symbols
to avoid sccache racing over a shared compiler PDB; Release flags are unchanged.
The diagnostic
commit is separate from the two proposed production changes.

The DSP comparison is pinned to:

- Baseline: `8c919d2b390fbe08fc6b26ab881cd80b427ec952`.
- Fixed: `61a486d620fd6404e2cab4a75febb26c9472cf03`.

Only `dsp56kBase/dspassert.h` may differ. Both builds use the same parent MSVC
flags and AsmJit revision. The suite runs six CTests in Release and Debug for
each version: the assertion contract, DSP assembler/JIT/interpreter/optimizer
suite, and four shared-buffer/ring-buffer/semaphore tests. The contract checks
that Release arguments/messages are not evaluated and that Debug failures reach
the handler while each expression is evaluated once.

The performance executable assembles independently authored ALU and memory
loops. It exercises single-block JIT dispatch, bounded JIT dispatch, and direct
interpreter entry. It contains no firmware, extracted fixtures, or audio data.
Direct interpreter entry in a JIT-enabled build does not advance the emulated
cycle counter; its instruction count and state remain part of the oracle.

For each case, baseline calibration chooses fixed work targeting two seconds.
That work is shared by 12 observations in A-B-B-A order repeated three times.
Each process warms its JIT before timing. Windows `GetThreadTimes` measures CPU;
the steady clock measures wall time. All observations must match DSP register/
memory checksums, instruction counts, cycle counts and PC. Timing is descriptive
because hosted runners can have noisy neighbors. The receipt also compares the
PE `.text` sections, which can reveal unchanged native code despite timing noise.

Artifacts include compiler flags, pinned source/binary hashes, host identity,
CTest XML, commands, raw observations and a report. The ordinary product checks
on hosted CI have no private firmware; any firmware test skips are not audio
validation. This suite does not establish MD/MM audio performance on Windows.
Apple ThinLTO/LLVM PGO options remain disabled on Windows, and native MSVC PGO
is not enabled by this experiment.

Local preparation verified CMake configuration, Release/Debug assertion behavior,
and deterministic synthetic cases using Apple Clang. Actual MSVC coverage comes
only from the Windows Actions run.
