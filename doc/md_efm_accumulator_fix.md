# EFM-SD and deterministic DSP execution

This update fixes the x86 DSP JIT defect that corrupts EFM-SD oscillator phase.
The reported settings are PTCH=64, DEC=96, NOISE=0, NDEC=64, MFRQ=64,
MDEC=127, HPF=0, velocity=110. MOD=0 loses the sustained tone; MOD=32 produces
broad noise instead of a tone. It also fixes related accumulator load, shift,
rotate, saturation and boundary-flag errors found by a targeted audit.

The dependency is [DSP PR #8](https://github.com/joelanders/dsp56300-md-mm/pull/8).
Its permanent coverage includes 54 explicit expected-value cases, 7,680
single-instruction interpreter/JIT comparisons, 488 sequence cases, and 1,168
independent DIV expected-result checks. It includes interpreter deferred-flag
corrections and an x86 overflow helper correction found during firmware tracing.
The plugin's existing Linux, macOS and Windows checks run the regression target.

## Long-sequence phase discrepancy

A broader MD matrix exposed phase differences after nine matching captures.
Repeated runs were deterministic within each architecture. The first lasting
execution difference was a host-to-DSP handoff at host cycle 2,124,120,003,
with DSP boot origin 20,003 host cycles. The exact destination deadline is
5,395,553,856 DSP cycles. The old floating frame calculation produced one cycle
less on x86 than on ARM because the hosts used separate versus fused arithmetic.
That changed subsequent emulated execution timing before the waveform diverged.

The scheduler now converts the elapsed integer host timestamp directly to a
floor-rounded DSP deadline, with overflow-safe whole/fractional arithmetic.
The remaining floating scheduler calculations disable reassociation and fusion
locally in `mdhardware.cpp`; DSP/JIT and other audio translation units keep their
existing compiler options. The conversion regression includes the actual traced
boundary, 900,000 reduced-ratio comparisons, and overflow/saturation cases.

The register trace also found an independent DIV overflow discrepancy. The x86
helper cleared emulated V using a host AND before reading the preceding arithmetic
condition, overwriting the host parity/zero flags. Capturing the condition first
fixes it in the DSP dependency. The new 24-iteration tests reproduce the firmware
operands; 352 of the initial 1,152 checks failed on the old x86 implementation.
All 1,168 final checks, including two additional single-step overflow boundaries,
pass locally on ARM and x86. This discrepancy was real even though its recorded
stack residue did not change the captured audio.

With both corrections, the initial ten-capture reproduction has matching audio
and execution checkpoints. Full MD/MM and physical Intel results are recorded in
the PR and durable investigation notes as validation completes. The scope is
bounded: this is not an exhaustive ISA, optimizer or hardware waveform proof.

The integration includes alpha.10's project-restore tests. Previous real Device
state encode/restore/save checks required unchanged parameters and audible MD
EFM-SD/MM FM+STAT output, including MM user flash. Headless Device and built-VST3
checks do not constitute an interactive Ableton session. No setting, firmware
change, profiling capture or migration is required. Installed plugins change
only when a build containing these corrections is installed.
