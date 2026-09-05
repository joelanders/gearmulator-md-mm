# MD/MM firmware-hook remediation

Prepared 2026-09-05 on `refactor/md-mm-firmware-hooks`, based on merged release
commit `65fe402deb87b60e279378f164aef620d33e672f`. The DSP submodule starts at
`363d3fc0632392a4cc9329cf5fd6e9f53e7a8ff6`.

Status: investigation and incremental implementation. The synthetic DSP2 boot
reply/command-vector deferral, MM firmware-PC sample correction, private
parameter-memory guard, and factory-waveform constructor injection have been
removed. Hardware-derived host-command replacements have passed the focused
JIT regression matrix below.
Interpreter audio parity and the remaining panel hooks are still unresolved.
The existing MD/MM behavior
is the comparison baseline, not proof that the hooks accurately model hardware.
This document does not assess legal permissibility or establish clean-room
provenance. Moving code into another module does not change how it was derived.

## Objective and scope

Replace host-side knowledge of firmware program locations, private data
structures, and algorithms with justified hardware or external-protocol
behavior. Keep each removal separately reviewable and record its evidence.

Initial scope is MD/MM and its DSP core integration. The inherited Nord Lead 2x
master-tune implementation and its analysis document are separate findings;
changing another synth's behavior should be a separate work item. Ordinary
memory maps, instruction implementations, MIDI messages, and panel protocols
are not automatically candidates for removal merely because constants appear
in their implementations.

## Findings and replacement work

| Case | Current location | Replacement direction | What remains unresolved |
| --- | --- | --- | --- |
| MM sample-buffer correction | Removed from DSP core and MD/MM constructor | Use ordinary instruction/peripheral execution; retain the six-track sine and SRR regressions. | JIT removal evidence is positive on ARM64/x86-64. Interpreter audio fails with the hook enabled or disabled; parity and the historical cause remain unresolved. |
| Panel-ready task-list updates | `mdmc.cpp`, `panelDisplayReadyPost` and its periodic caller | Have the emulated panel/peripheral signal readiness through the proper external interface, allowing firmware to update its own task lists. | The correct readiness signal and timing must be established; do not assume that sending an arbitrary UART byte replaces the semaphore update. |
| MM parameter-transfer ordering | Private-memory guard and tracking removed from `mddsp.cpp` | Two-stage receive pacing plus HCIE-gated, source-tagged commands, atomic wake, and shared host/DSP acceptance/cancellation. New commands no longer wait for handler return. | Sine/burst and Ensemble pass ARM64/x86-64 JIT without the guard. Clamp fallback, return tracking used by empty-host-read progress, full device reset/state restore, and wider concurrency still need audit. MD retains legacy pacing after its UW regression failed with two-stage pacing. |
| Factory DigiPRO waveform injection | Constructor copier and `mdmmwaveforms.h` removed | Let the supplied firmware run through the existing emulated processors/peripherals; test DigiPRO via documented MIDI controls. | DPRO-DDRW and DPRO-DENS JIT sweeps cover all six tracks and the full waveform-selector CC range. Exact waveform identities/spill equivalence, edited banks/state restore, and physical-hardware comparison remain unverified. |
| Synthetic DSP boot response | Removed from `mddsp.cpp` | Use the ordinary emulated host-command and RX/TX paths, letting the supplied firmware execute. | MD and MM firmware regressions pass without interception. Broader hardware equivalence remains unproven. |
| Panel startup handshake | `mdmc.cpp`, `onPanelTransmit` | Encapsulate the absent panel controller as an external-protocol device with explicit reset/startup states. | Establish provenance of the protocol description. This may be appropriate protocol emulation already; it should not be conflated with direct task-list rewriting. |

The panel and boot code's comments attribute behavior to MAME. Those comments
are leads for a source/provenance check, not independent verification that the
current implementation matches a particular public driver revision.

## Investigation results: synthetic boot response

The supported MM 1.32b fixture is available locally (SHA-1
`11a37460a5f47fd1a4d911414288690e6e7da605`). Tests use user-supplied firmware;
no firmware image or extracted content is added to the repository.

Removed the DSP2 query-word interception and model-specific constant replies,
then separately removed the vector-specific deferred command dispatch and its
state. Commands now follow the same existing host-command path as other
vectors, and argument words use the ordinary paced receive path. No replacement
firmware address, query signature, private-memory read, or synthesized reply was
introduced. Both experimental stages passed the firmware tests.

Added `mmBootFirmwareTest`: validate the supported ROM, advance through boot,
require DSP and panel/MIDI readiness, and require a kit-status response generated
by the firmware through the MIDI UART. Merely producing finite silence is not
sufficient to pass this test. The fixture is optional (CTest skip code 77).

Native ARM64 Release validation with the interception and deferral both absent:

- `mdUwFirmwareTest`: passed (36.63 s), including mode and RAM audio checks.
- `mmBootFirmwareTest`: passed (11.70 s), kit-status response received.
- `mdAudioFirmwareTest`: passed with both fixtures (8.48 s).

These results support removal for the pinned supported images; they do not
establish physical-hardware timing equivalence, all MM sound engines, reset/state
restore coverage, or other firmware revisions. Parameter-transfer ordering and
other existing hooks were still enabled and are separate unfinished work.

Provenance check on 2026-09-05: the current public
[MAME Elektron driver](https://github.com/mamedev/mame/blob/master/src/mame/elektron/elektronmono.cpp)
is a non-working skeleton, not the detailed panel/task-list implementation
claimed in local comments. Those attributions remain unverified; this check
does not exclude a different historical revision or fork. Do not cite the local
comments as evidence that these techniques came from public upstream MAME.

## Panel dependency experiment (2026-09-05)

On top of the boot-hook removal, temporarily removed only the periodic caller
of `panelDisplayReadyPost`, leaving all UART startup replies and other emulation
unchanged. Native ARM64 Release results:

- `mdUwFirmwareTest` failed after 9.92 s: `firmware did not initialize UW flash`.
  This test starts without a reusable factory flash cache. Failure occurs before
  the later menu/LCD checks, demonstrating a first-run dependency, not merely a
  cosmetic display regression.
- The original `mmBootFirmwareTest` passed after 11.76 s. Its boot/MIDI checks
  alone did not establish continued LCD progress.

Strengthened the MM test to enter and leave the tempo menu three times and
require changing LCD contents on every transition. It passes with the periodic
caller both present and absent. This is an external-interface progress smoke
test, not a pixel-perfect hardware display oracle or coverage of every menu.

Restored the periodic caller for MD only; MM no longer attempts these MD
private-memory updates. The MD panel-memory hook remains unresolved, so this is
not completion of panel remediation. Do not replace it with an arbitrary UART acknowledgement just to
make boot pass: readiness signaling still needs evidence from an external panel
trace, an independently documented protocol, or a demonstrated peripheral bug.

Public web and GitHub code searches for the named helper did not locate a
relevant upstream implementation. This is limited negative evidence, not proof
that none exists. Corrected misleading MAME attribution comments in the panel
code and LCD decoder. The public MAME driver itself labels some hardware details
as guesses based on firmware; its availability is not clean-room provenance.

Final regression run with the MD-only caller: `mdUwFirmwareTest` (37.31 s),
strengthened `mmBootFirmwareTest` (13.27 s), `mdLibTests`, `mdAudioQueueTest`, and
`mdRamAudioOracleTest` all passed. The new MM test still returns skip code 77
when its firmware fixture is absent.

## MM audio reproduction coverage (2026-09-05)

Added optional `mmAudioFirmwareTest`, using fresh hardware without host-supplied
patch RAM. It requires quiet idle output, triggers each of the six MIDI tracks,
checks finite/non-silent main stereo output, and checks that setting each track's
level to zero attenuates its output by at least 20 dB relative to level 127.
The test uses ordinary MIDI note and CC7 traffic. Elektron's
[Monomachine manual, Appendix B](https://device.report/m/005f7a00dcf4647e0966bafc63a858c05ab53ca0f4455912fd719350a415a34f)
documents CC7 as per-track level on the six consecutive track channels.

The baseline passes for all six tracks. Audible RMS ranges from 0.0119 to 0.0892;
zero-level RMS ranges from zero to 0.0000598 (remaining effects tails are allowed).
This establishes note/level responsiveness, **not** correct waveform shape,
sample-rate reduction, DigiPRO bank population, or rapid parameter ordering.

An earlier note-only version gave the same printed six-track RMS values with
the MM sine correction enabled and disabled. That experiment did not establish
coverage of the correction's targeted case; the correction was restored. Do not
interpret this green smoke test as evidence that the sine hook can be removed.

Separately bypassed only the private-memory parameter-block guard in
`writeWordToDsp`. The six-track note/level test still passed, with identical
printed RMS values. Restored that guard too: sequential level changes do not
reproduce the documented special-transfer ordering case, and its removal needs
a stronger stress/parameter fixture. No runtime audio or parameter hook was
removed by this coverage commit.

With both hooks restored, the final native ARM64 Release CTest run passed in
31.25 s. With no MM firmware environment variable, the executable returns 77.

The manual's kit-loading and machine-clearing instructions provide an external
control route to a GND>SIN test setup, implemented in the follow-up below. Neither
boot readiness nor a nonzero RMS is an adequate substitute.

## Targeted GND>SIN reproduction (2026-09-05)

Added `mmSineFirmwareTest` (`mmAudioFirmwareTest --sine`). It clears and loads
a kit using KIT > LOAD, FUNCTION+PLAY, ENTER, EXIT. These are the manual's
ordinary panel operations on a newly constructed test machine, not firmware
memory patches. A diagnostic LCD capture confirmed `NEW KIT` and `GND>SIN`;
the temporary capture code was removed. No project, firmware bytes, or waveform
recording is checked in.

The test exercises all six tracks at MIDI notes 36, 48, 60, 72, and 84. It checks
finite/non-silent output and normalized second-difference energy after onset
settling. For an ideal sine this is `(2 - 2*cos(2*pi*f/sampleRate))^2`; a factor
of four tolerance detects DC, octave-scale pitch errors and large sample
discontinuities without depending on exact emulated sample bits. This is not a
full spectral or physical-hardware waveform oracle. At note 60, CC82=64 must
produce an audible, measurably different reduced-rate signal; CC82=0 restores
the nominal setting. Appendix B of the same manual documents CC82 as SRR.

`mmSineOracleTest` is firmware-independent: an ideal synthetic sine must pass;
silence, DC, a sine with every sixteenth sample zeroed, NaN and infinity must
fail. It uses the same difference-energy calculation as rendered audio.

Experiment results on native ARM64:

- Hook enabled: all six tracks at all five pitches are smooth. Note 60
  normalized difference energy is about `1.93e-6`; note 36 about `7.75e-9`,
  note 84 about `4.92e-4`. SRR=64 at note 60 gives about `1.4e-5`.
- Hook disabled: the targeted test passes with both the default bounded-JIT
  scheduler and `GEARMULATOR_MDMM_BOUNDED_JIT=0`. Printed RMS and difference
  metrics match the enabled run at their reported precision.

This is stronger evidence that the correction may now be obsolete for the
supported image, not a reproduction of the historical defect. Both scheduler
settings still use JIT; they are not interpreter parity coverage. The hook was
restored pending core removal validation, including other execution modes and
architectures. No claim is made that these observations explain why it was
originally introduced or prove equivalence for every synthesis setting.

Final native ARM64 Release validation with the hook restored: existing
`mmAudioFirmwareTest` passed (31.84 s), `mmSineFirmwareTest` passed (43.11 s),
and all synthetic controls in `mmSineOracleTest` passed (0.01 s).

## Cross-architecture and interpreter investigation (2026-09-05)

Built separate Release trees with the sine correction disabled at its MD/MM
call site. Verified the x86-64 executable format and the interpreter build's
`DSP56K_FORCE_INTERPRETER=1` compile definition.

- x86-64 under Rosetta: `mmBootFirmwareTest` passed (20.62 s),
  `mmSineFirmwareTest` passed (64.36 s), and `mmSineOracleTest` passed (0.04 s).
- ARM64 forced interpreter: both firmware tests timed out at 180 s each;
  the firmware-independent sine oracle passed. This is **not** interpreter
parity validation.

The separate hook-enabled boot comparison also reached its 180-second alarm
deadline (exit 142), rather than completing. All validation processes for this
experiment have terminated; there is no unfinished background test to resume.

A separate hook-enabled interpreter boot comparison exposed a scheduling
problem before synthesis setup. A one-second stack sample remained in
`Hardware::processUC` -> HDI host write -> `Dsp::writeWordToDsp` -> DSP `DOR` ->
`DSP::do_exec`, polling HI08 status. Source inspection confirms that `do_exec`
executes the entire loop recursively before returning. The MCU cannot provide
the next host word while its current callback is executing that polling loop;
the caller's cycle clamp is checked only outside the non-returning DSP call.
This is a host/DSP scheduling issue present with the correction enabled, not
evidence that the removed sine correction is needed for boot.

Next: reproduce this with an independently assembled peripheral-polling DO/DOR
program, then make interpreter loop execution cooperative with the host
scheduler while preserving architectural loop state, nested loops, interrupts,
and cycle accounting. Use the public
[DSP56300 Family Manual](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
for DO/DOR/ENDDO semantics, not firmware-private program locations. The sine
hook remains restored while this validation gap is investigated.

Separate factory-bank experiment: bypassing only constructor waveform injection
still passes the original six-track factory-kit note/level test, with the same
printed RMS values. The sine correction was enabled for this experiment. This
does not prove DigiPRO bank correctness; an explicitly selected DigiPRO fixture
is still required. Constructor injection was restored.

## Cooperative interpreter loop fix

DSP commit `50ff78f0` on the DSP repository's `refactor/md-mm-firmware-hooks`
branch changes DO/DOR setup to establish LA/LC/stack state and return. Loop-end
processing now happens after individual interpreted instructions, using the
last fetched instruction word and architectural LF/LA/LC state. This follows
the public manual's DO semantics; no firmware-specific address or algorithm
was added. DO setup cycles/instruction counts now use the ordinary `execOp`
accounting path, avoiding double counting.

Regression evidence:

- Before the fix, the new finite-loop check failed because one interpreter
  step had already executed the whole loop instead of stopping after setup.
- A separately assembled loop polls a host-supplied X-memory flag. After setup,
  the host can run bounded DSP steps, supply the flag, and let the loop finish.
- Added nested-loop state restoration, early ENDDO, and zero-count skip checks;
  the cycle test checks DO setup separately and the total completed-loop cost.
- Full DSP suites pass in forced ARM64 interpreter, ARM64 JIT, and x86-64 JIT
  builds. Those JIT builds also execute the interpreter unit suite.
- With the fix and the sine correction still enabled, MM interpreter boot,
  MIDI response, and repeated LCD checks pass in 46.64 s (previously timeout).

**Remaining interpreter failure:** `mmSineFirmwareTest` now reaches a failing
audio check rather than deadlocking: out-of-range DSP reads appear and idle
audio is unexpectedly nonzero after clearing/loading the kit. Do not describe
this as full interpreter audio parity, or remove the sine hook on that basis.
The loop fix addresses the independently reproduced scheduler deadlock only.

Further source inspection identified memory-operand DO handlers using
`effectiveAddress` as the count where the JIT uses `readMem`. This is a candidate
instruction discrepancy to test against the manual, not yet an established
cause of the MM audio failure. Investigation should use synthetic instruction
tests before drawing firmware-level conclusions.

Final ARM64 JIT firmware validation with this DSP revision: `mdUwFirmwareTest`,
`mmBootFirmwareTest`, and `mmSineFirmwareTest` all passed (51.44 s wall-clock
with two tests scheduled concurrently). The MD RAM/mode and MM sine/panel guards
remain intact. No firmware-specific runtime hook was removed by this loop fix.

## Memory-source loop counts and sample-hook removal

DSP commit `717dd7cb` corrects another independently reproduced ISA discrepancy:
memory-source DO (direct and effective-address forms) and short-address DOR must
load LC from memory, not use the source address itself. Sixteen synthetic cases
cover DO/DOR, X/Y selection, direct/(R0)+ addressing, zero/nonzero counts, one
post-increment, and completed-loop state. The old implementation fails the
zero-count case; all cases and the full three-build DSP suites pass after the
fix. The already-correct DOR effective-address form is covered too.

This count correction **does not fix** the MM interpreter audio failure.
Afterward, the sine test fails with the same out-of-range reads/non-silent idle
output with the correction enabled (45.92 s) or disabled (46.33 s). The factory
kit audio test also fails with it disabled, without clearing/loading a sine kit.
Therefore this is not evidence of a sine-specific regression or of interpreter
audio parity. The generic instruction fix is retained on its own evidence.

DSP commit `13295e64` removed `dspMmCleanGndSinStep` completely, including its captured lanes, pending
state, reset hooks, setter/accessor, pre-step callbacks, and bounded-dispatch
exception. MD/MM no longer enables it. There is no replacement firmware-PC or
private-variable condition: the supplied firmware runs through the ordinary
DSP instruction/peripheral path. This removes the live dependency, not its Git
history or the need to investigate provenance.

Removal is supported by the targeted ARM64/x86-64 JIT comparisons already
recorded above. It is **not completion of all audio-remediation acceptance
criteria**: forced-interpreter audio remains broken on both sides of removal,
and physical-hardware waveform equivalence is not established. Keep these gaps
visible instead of retaining an unsubstantiated private-memory repair or
declaring the entire goal finished.

Final removal validation:

| Build | DSP core suites | Firmware results |
| --- | --- | --- |
| ARM64 JIT | All pass | MD UW/RAM/modes (38.13 s), MM boot/panel (14.01 s), factory audio (31.97 s), sine/SRR (44.03 s), and sine oracle pass. |
| x86-64 JIT / Rosetta | All pass | MM boot/panel (21.49 s), sine/SRR (66.18 s), and sine oracle pass. |
| ARM64 forced interpreter | All pass | MM boot/panel passes (49.31 s). Sine audio fails (46.88 s) with the same invalid reads/non-silent idle output after full removal. |

Source search confirms the deleted hook API, state, and function no longer
occur in the DSP core or MD/MM runtime sources.

## Parameter-ordering dependency reproduced

Strengthened `mmSineFirmwareTest` with 16 rounds of SRR changes across all six
voices while each tested voice is sounding, ending at nominal SRR everywhere.
After allowing MIDI traffic to drain, rendered audio must again satisfy the
sine check. This tests the resulting DSP audio state, not only MCU-side kit
metadata. Setup and changes still use panel operations and documented CC82.

Temporary diagnostics in the existing guard showed that the command/word
qualification was reached for every DSP/voice combination. The private-memory
wait actually ran for DSP index 0, local voices 1 and 2, in both the original
sweep and the strengthened run. Thus this is not merely a passing test which
never reaches the workaround. Those diagnostics have been removed.

Removed the guard, its word/voice tracking, and its command-vector tracking as
an experiment. On both ARM64 and x86-64 JIT, the strengthened test fails at
zero-based track 4 (track 5): RMS is `1.87984e-7` and the sine check reports
silence. The guarded ARM64 baseline passes. MM boot remains successful without
the guard, confirming why boot-only validation would miss this regression.

At that experiment's conclusion, restored the guard and its tracking state.
The test became a concrete acceptance gate for a replacement
based on HI08 receive buffering, host-command handling, or scheduling. Do not
replace the private variable check with a different firmware address or merely
remove the failing audio assertion. The correct underlying fix is not yet
established by that removal-only experiment. The next experiment is below.

Removed the stale boot-acknowledgement state comment and replaced an unverified
MAME attribution in the paced receive-path comment with a description of the
actual local behavior. No runtime workaround is removed in this test change.

With the guard restored and temporary diagnostics absent, the strengthened
test passes on ARM64 (44.40 s) and x86-64/Rosetta (67.01 s). This supplies a
red/green removal comparison on both JIT architectures.

## Two-stage host receive pacing experiment

The [DSP56303 User's Manual, section 6.7.3, table 6-17](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
distinguishes the host transmit latch from the DSP receive latch: TXDE reports
room in the former; TRDY requires both to be empty. Our MCU-visible ISR already
models those two stages using receive queue depth. However, `writeWordToDsp`
waited for the entire receive queue to empty before accepting each word. That
unnecessarily advanced the DSP while the host should still have been able to
fill its own latch.

Changed pacing to wait only at depth two or greater, and disabled the private
parameter guard. The ARM64 boot/panel test passes (12.98 s) and the strengthened
six-track sine/SRR/burst test passes (42.80 s). This contrasts with the earlier
guard-removal-only failure. The candidate therefore removes the private-memory
read, voice/word tracking, and command-vector classification entirely; it does
not replace them with a different firmware address or a delay constant.

This is evidence for the pacing correction, not a complete HI08 model. The
existing cycle-clamp fallback can still enqueue beyond the two hardware stages.
Command serialization also deserves separate testing: `hostCommandBusy()`
includes handler execution until return, whereas the hardware command-pending
bits clear when the interrupt is accepted. DSP HCP and the MCU CVR are updated
through different paths in the current implementation. Neither behavior is
silently changed as part of the receive-pacing experiment. Physical hardware
equivalence remains unverified.

Applying the same pacing change to MD stalls the UW test on both JIT builds.
Both logs report the emulator's invalid-PC trap; an ARM64 process sample shows
the main thread sleeping inside `DSP::onInvalidPC`, reached from MCU host
transmission through `schedCatchUpDsp`. The two test processes were terminated
after capturing the evidence, rather than waiting for the 900-second timeout.
Consequently the implementation enables two-stage pacing for MM only and
preserves MD's prior drain-before-write behavior. This is an incremental
compatibility boundary, **not** evidence that real MD hardware has only one
stage. The MD transport/scheduling discrepancy still needs investigation.

Forced-interpreter MM boot/panel passes (46.86 s), but sine audio still fails
with non-silent idle output (46.51 s), matching the earlier interpreter failure.
The pacing change does not establish interpreter audio parity.

With the private parameter guard and all tracking code deleted, MM JIT
boot/panel passes on ARM64 (13.57 s) and x86-64/Rosetta (20.59 s), and the
strengthened sine/SRR/burst test passes on both (44.92 s / 67.42 s).
ARM64 factory audio (31.19 s) and the sine oracle also pass. After restoring
legacy pacing specifically for MD, its UW/RAM/mode regression passes on ARM64
(38.30 s) and x86-64/Rosetta (57.70 s); the randomized audio test passes (8.43 s).
The original broad-pacing runs remain recorded as failed/terminated MD tests,
not successful runs.

The final MM-only implementation also passes the strengthened sine/SRR/burst
test with ARM64's legacy JIT scheduler (`GEARMULATOR_MDMM_BOUNDED_JIT=0`,
45.41 s). This check changes scheduler mode, not instruction interpreter mode.
The rebuilt final x86-64 bounded-JIT executable passes the same test (67.12 s).

## DigiPRO injection removal investigation

The [published Monomachine manual](https://www.bhphotovideo.com/lit_files/85386.pdf)
provides the needed external controls: Appendix C assigns DPRO-DDRW with SysEx
command `0x5b`, machine number 32, and data-page initialization; Appendices A/B
describe WAV1/WAV2 and their synthesis-parameter CCs (48/50). DPRO-DDRW uses the
64-wave MKII bank, unlike the older DPRO-WAVE machine's separate 32-wave set.

Added `mmDigiproFirmwareTest` (`mmAudioFirmwareTest --digipro`). It starts with
a fresh empty kit, assigns DPRO-DDRW to each of the six tracks in turn, checks
track-level attenuation, and sweeps both selectors together through all 128
MIDI values. Each observation must produce finite, audible audio; each track's
normalized second-difference energy must vary by more than a factor of two
across the sweep. This is a timbre-response check, not an assertion of exact
factory waveform identities or the CC-to-slot scaling formula.

The companion `mmDigiproEnsembleFirmwareTest` assigns DPRO-DENS (machine 33)
and sweeps its WAVE control (synthesis parameter 4, CC51) in the same way.

The initial held-note experiment decayed to silence during the first sweep.
Retriggering each observation avoids confusing the default amplitude envelope
with waveform loss. A temporary LCD capture after assignment visibly showed
DPRO-DDRW and WAV1/MIX/WAV2/TIME controls. The capture helper is removed; no
firmware image, waveform bank, or LCD asset is added to the repository.

The ARM64 injected baseline passes all 768 selector observations. Temporarily
omitting only the constructor copy also passes all 768 on x86-64/Rosetta. This
is stronger evidence than the earlier factory-kit test, which never explicitly
selected a MKII DigiPRO machine. No alternate preload or private-memory write
was added: normal emulation supplies the data needed for this playback test.

Removed the constructor copy and its fixed source/destination-layout helper.
Also removed the helper-specific synthetic copy tests, which asserted that same
private layout; retained the strict supported-image size/fingerprint tests.
The new MIDI/audio regression exercises the user-visible behavior instead.
The source history still preserves the removed implementation and its tests.

After full code removal, ARM64 passes strict image validation (0.24 s), MD
UW/RAM/modes (36.89 s), MM boot/panel (13.21 s), sine/SRR/parameter bursts
(43.63 s), DPRO-DDRW's complete sweep (119.93 s), and the sine oracle. Comparing
the 768 printed `(track, CC value, RMS, roughness)` observations with the
injected ARM64 baseline finds no differences at the printed precision. This
is stronger than finite output alone, but is not a sample-by-sample bitwise
comparison or an independent physical-hardware oracle.

Full-removal DPRO-DENS passes on ARM64 (119.72 s). On x86-64 it fails after
the fifth track's sweep: the initial loud-note render for that track had RMS
`5.16275e-8`, although all its subsequent retriggered selector observations
were audible. This is an additional acceptance failure, not a successful
cross-architecture DPRO-DENS result. Restoring the constructor injector on
x86-64 produces the same failure (151.28 s), at the same track with exactly
the same initial-note RMS. Thus removal does not cause this observed failure;
the new test exposes an existing x86-64 acceptance issue. The failing test
remains enabled rather than being marked as expected failure or having its
audio assertion relaxed. No private-memory guard is reintroduced to hide it.
All 645 printed selector/track audio-metric lines in the two x86-64 DPRO-DENS
runs match exactly, including the failing observation. The next diagnostic is
to isolate the x86-64 initial-note failure using these same public commands,
not to reinstate the bank copier.

This does not yet establish exact bank/slot contents, the four formerly copied
spill words, edited/user banks, state restore, DPRO-DENS behavior, interpreter
audio parity, or physical-hardware equivalence. Those remain acceptance work;
passing finite/timbre checks is not a claim that every bank-related behavior is
now correct.

## Ensemble transport regression narrowed

The x86-64 DPRO-DENS failure also occurs with the legacy JIT scheduler
(`GEARMULATOR_MDMM_BOUNDED_JIT=0`, 157.72 s), at the same fifth-track initial
note with RMS `5.16275e-8`. Temporarily skipping selector sweeps while retaining
machine assignment, level checks, and initial notes passes all six tracks.
Thus the earlier sweep history is relevant; basic Ensemble assignment alone
does not reproduce the failure.

Temporary HI08 diagnostics found no receive-write or handler-return clamp
exhaustion in the failing full sweep. Both DSPs did reach the pre-command
receive-drain clamp. Those diagnostics were one-shot, so they do not yet prove
whether that drain behavior contributes to the later silent note. Diagnostics
are removed from runtime source.

An important wider regression comparison: building `mddsp.cpp/.h` from
`a4a84ea9` (old one-stage pacing **and** private parameter guard) with the new
Ensemble test passes all six tracks on x86-64. Factory waveform injection
remains absent in that comparison. Consequently the new receive-pacing
replacement is **not fully validated**: the failure predates waveform-injection
removal, but is a regression relative to the old parameter transport. The
temporary old guard is not retained in runtime source. Further comparisons
must separate old pacing from the private guard before selecting a hardware-
based correction; machine-specific pacing or restoring a private-memory read
would not complete remediation.

The initial note/attenuation assertions now run before each long selector
sweep. This reports the same failure sooner and preserves successful test
behavior; no assertion is weakened or marked as expected failure.

The separated x86-64 comparisons establish this matrix (all use the same
published-command Ensemble test, with factory injection absent):

| Receive pacing | Private parameter guard | Ensemble result |
| --- | --- | --- |
| Drain before each word | Present | Pass, all six tracks. |
| Drain before each word | Absent | Pass, all six tracks. |
| Two-stage pacing | Present | Pass, all six tracks. |
| Two-stage pacing | Absent | Fail, fifth-track initial note silent. |

Thus the guard is unnecessary for this case under old pacing but does prevent
the failure under two-stage pacing. Conversely, the earlier sine/SRR burst
test fails with old pacing and no guard. Selecting pacing by synthesis machine
would only conceal these two incompatible acceptance results, not explain or
fix the emulated host interface.

Also tested omitting the MM pre-command receive drain. The public HI08 TRDY
description allows host data to wait for a subsequently issued command's
handler, so indiscriminate draining warrants review. However, removing that
drain alone stalls x86-64 MM boot: a process sample shows the single emulator
thread blocked in `HDI08::writeRX` / the 8,192-word receive ring's condition
variable, reached through the MCU host-write callback. The existing pacing
clamp fallback can thus accumulate data until a thread-blocking queue fills.
The runner and stalled child were stopped after capture; the Ensemble test
was not run in this configuration. The drain is retained in source. A proper
solution must address the coupled command/data scheduling and bounded-queue
behavior, not merely delete this wait or add an arbitrary delay.

After restoring the current runtime source and rebuilding, x86-64 MM boot/panel
(19.78 s) and the sine oracle pass. The only committed code change from this
diagnostic round is earlier reporting of the existing audio assertions. The
two-stage/no-private-guard Ensemble failure remains an active gate; this branch
is not ready to merge on the strength of the earlier sine-only pacing evidence.

## Firmware-free host-command enable regression

Added `mdHdi08HardwareTest`, a DSP56303 peripheral fixture with a tiny synthetic
main loop and interrupt handler. It loads no firmware, constructs no MD/MM
hardware object, and uses no private firmware variables or command payloads.
The handler merely writes a marker to X0 and returns. An enabled-command
positive control must execute that handler. With HCIE clear, HCP must remain
pending without executing the handler; enabling HCIE later must deliver it.

The [DSP56303 User's Manual, section 6.6.1, table 6-8](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
specifies that HCIE enables the interrupt request associated with HCP. The
current optional host-command arbitration path ignores HCIE when injecting a
command, and the core's interrupt-mask check considers processor priority,
not this peripheral enable bit.

The execution-based test passes its enabled positive control and then fails
on both ARM64 (0.16 s) and x86-64 (0.43 s): the handler executes with HCIE=0.
This is stronger than asserting a particular internal queue representation.
The synthetic handler's short immediate MOVE uses the documented high-byte
placement in X0; the test checks that marker correctly. The test is enabled,
has a ten-second timeout, and needs no ROM environment variable.

This establishes a hardware-model defect independently of the Ensemble
failure; it does **not** yet establish that defect as the Ensemble root cause.
The pending-command/enable transition is an acceptance gate for the eventual
fix. Also test disabling or cancelling a request before service and ensure a
disabled source cannot block unrelated interrupts. Do not implement a fix
that drops commands issued while disabled or only gates their initial enqueue
while ignoring later enable changes. No runtime correction is committed in
this test-only step, and the remediation branch still has failing gates.

## Host-command enable correction and scheduling validation

The candidate correction represents a CPU interrupt request with its optional
peripheral source and a generation token. Untagged callers retain their old
behavior. HI08 holds HCP while HCIE is clear; its DSP-side peripheral execution
or HCR write queues a request when enabled. The CPU checks source eligibility
again before service. A request disabled in the meantime is withdrawn without
discarding the peripheral's pending command or blocking other CPU requests.
Only service of the matching tagged request acknowledges HI08; another source
using the same vector cannot clear HCP. Tokens invalidate queued requests when
arbitration is reconfigured. Sources must outlive their queued requests.

The host thread publishes command state and requests peripheral execution; the
DSP owner enqueues the CPU request. This avoids creating an additional
producer for the external interrupt ring. Existing vector-only observers are
kept separate from source acknowledgement. The optional arbitration-disabled
legacy path is not changed as part of this MD/MM correction.

Review removed a new host-side `setDelayCycles(0)` call: that scheduling API
mutates non-atomic DSP-owner counters. Simply omitting the wake passed the
synthetic tests and all six ARM64 gates, but reproduced the x86-64 Ensemble
failure at track 4 (initial RMS `5.16275e-8`, 227.68 s suite total). Thus HCIE
gating alone is insufficient: prompt peripheral scheduling is a separate
requirement exposed by this regression.

The replacement wake API publishes an atomic request and an atomic due-clock;
it neither reads the DSP instruction counter nor writes the owner's delay.
Peripheral execution consumes the request before processing, and rescheduling
preserves any unconsumed/new wake. The rescheduling exchange acquires preceding
host publications before checking the request flag. The existing JIT due-clock
load remains a naturally aligned 64-bit load. Synthetic tests cover waking,
rescheduling before consumption, consumption, and a wake during processing.
This is not a claim about exact physical interrupt latency or a certification
that existing transport concurrency, queue clamping, and serializer behavior
are race-free. Validation of this atomic-wake candidate is recorded below.

Expanded the synthetic test to cover disabling an already queued request,
unrelated interrupts proceeding, subsequent re-enabling, no duplicate service
after enable toggles, same-vector source ownership, and stale queued requests
after reconfiguration. Masking the CPU at IPL 3 first ensures that the queued-
request cases genuinely reach the queue before testing withdrawal/reset.

The initial tagged-request candidate passes all ARM64 core/HI08, MD UW/RAM,
MM boot, sine/burst, and Ensemble tests. On x86-64 it passes both the sine/burst
test (68.03 s) and the previously failing complete Ensemble test (207.86 s).
This is a hardware-derived candidate that satisfies both formerly incompatible
audio cases without the private guard. Final validation follows the DSP-owner
enqueue refinement; these preliminary results are not substituted for it.

Host-side CVR cancellation and the existing handler-return serializer still
need separate audit; this correction does not claim a complete HI08 model.

The no-wake candidate also passed interpreter MM boot (48.67 s), but interpreter
sine failed its idle-silence check (48.41 s), as before. Correct host-command
enable handling alone does not establish interpreter audio parity.

A follow-up dependency experiment removed only the MD periodic panel task-list
post on top of the HCIE correction. `mdUwFirmwareTest` still failed with
`firmware did not initialize UW flash`; the same correction with the post
present passed its full UW regression. The temporary removal was reverted.
Correcting host-command enable handling therefore does not, by itself, replace
the MD panel readiness mechanism.

### Atomic-wake validation and retained limits

DSP commit `5283572a` contains the final HCIE/source-ownership correction and
atomic wake API. No private guard, waveform copier, or firmware-PC audio hook
was restored. Final Release results for this exact runtime code:

| Gate | ARM64 JIT | x86-64 JIT | ARM64 interpreter |
| --- | --- | --- | --- |
| DSP core unit suite | Pass, 1.84 s | Pass, 2.44 s | Pass, 1.77 s |
| Synthetic HI08/scheduling tests | Pass, 0.18 s | Pass, 0.46 s | Pass, 0.18 s |
| MD UW/RAM/modes | Pass, 37.13 s | Not rerun in this increment | Not run |
| MM boot and repeated panel interaction | Pass, 13.83 s | Pass, 20.87 s | Not rerun after atomic-wake refinement |
| Six-track sine/pitches/SRR/burst | Pass, 46.73 s | Pass, 69.98 s | Not rerun after atomic-wake refinement |
| Full six-track DPRO-DENS sweep | Pass, 125.22 s | Pass, 184.62 s | Not run |

The x86-64 full Ensemble sweep also passes with
`GEARMULATOR_MDMM_BOUNDED_JIT=0` (legacy scheduler). The standard ARM64 suite
passed 6/6 and x86-64 5/5; interpreter core/synthetic checks passed 2/2. The
earlier interpreter boot pass and audio failure above are from the explicitly
identified no-wake candidate, not silently counted as final-candidate results.

This resolves the conflicting JIT sine/Ensemble acceptance cases that prevented
the two-stage receive change alone from being a supported replacement. It does
not prove a fully accurate HI08 model, resolve interpreter audio, remove the MD
panel task-list hook, establish panel-protocol provenance, or complete this
remediation branch. In particular, quiescent arbitration reconfiguration is
tested; complete device reset/state restoration and host CVR cancellation are
separate acceptance work.

## Host-command reset regression

After `5283572a`, an independent synthetic test found that `HDI08::reset()`
cleared the stored status register but left the command serializer pending.
`readStatusRegister()` consequently reasserted HCP after reset. The ARM64
baseline failed with `hardware/software reset retained HCP` (0.26 s).

[DSP56303UM section 6.6.9, table 6-13](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
specifies HCP = 0 after hardware and software reset. The correction reuses the
existing arbitration reconfiguration invalidation during reset: pending,
injected, in-flight, and extra-queued command state is cleared, and generation
tokens invalidate old CPU requests. The arbitration configuration is preserved.
This assumes a quiescent host for reset/reconfiguration; it does not introduce
or claim concurrent reset support.

The regression covers both HCIE-disabled peripheral-only pending state and an
HCIE-enabled request queued behind CPU IPL 3. In each case it also queues a
second command, resets the port, verifies cleared HCP/busy state, re-enables the
port, checks that neither old handler executes, and verifies a fresh command.
This is synthetic peripheral reset coverage, not whole-device state restoration.
Individual reset via HEN and STOP, other HI08 register/FIFO reset details, and
host-side CVR synchronization remain separate work.

Reset-increment validation: ARM64 core/synthetic/MD UW/MM boot passed 4/4
(2.07/0.20/37.40/13.26 s); x86-64 core/synthetic/MM boot passed 3/3
(2.61/0.49/21.08 s); interpreter core/synthetic passed 2/2 (1.86/0.24 s).
The full audio sweeps were not rerun for this reset-only change; the preceding
atomic-wake matrix remains the separately identified audio evidence.

The host-side CVR audit identifies the next transport mismatch:
`source/mc68k/hdi08.cpp` clears HC immediately after its IRQ callback returns,
and does not notify the DSP when a host write clears HC. Actual command service
may occur later (including while HCIE is disabled). The same manual's table 6-9
requires HC/HCP to track pending acceptance/cancellation. Merely clearing more
serializer state is not a complete fix: the host-visible register and DSP-side
pending request need a shared lifecycle. No host-side behavior was changed in
this reset increment.

## Shared host-command acceptance/cancellation

The next correction makes the ColdFire-facing CVR optionally derive HC from
the DSP's pending command state. A host IRQ callback returning is no longer
treated as acceptance in MD/MM. Clearing HC calls an owner-thread cancellation
operation, which invalidates queued CPU request tokens and pending serializer
entries, but does not abort an already accepted handler. The existing extra
serializer entry counts as pending in both HC and HCP until acceptance or
cancellation; it cannot be silently acknowledged just because delivery waits
for the earlier handler's return.

The public hardware basis is
[DSP56303UM table 6-9](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf):
HC/HCP clear on interrupt acceptance, and a host clearing HC clears HCP. The
MD/MM machine scheduler runs both processors on the calling thread with no
background DSP thread (`mdhardware.h`). CVR observation/cancellation catches the
DSP up to current machine time before accessing its command state. Cancellation
is an owner-thread operation, not an additional cross-thread producer API.

The generic MCU register implementation exposes an opt-in pending/cancel pair;
unconfigured callers keep the existing synchronous acknowledgement behavior.
Only MD/MM installs the callbacks. This requires a companion change on the
`refactor/md-mm-firmware-hooks` branch of `mc68k-md-mm`, in addition to the DSP
branch. It does not modify another synth's integration.

Expanded firmware-free coverage uses the actual MCU host register joined to
the synthetic DSP fixture: HCIE-disabled retention, CPU-masked queued delivery,
acceptance, cancellation, retained HV, matching HC/HCP, and legacy behavior.
Another case stops between acceptance and interrupt return, queues a second
command, verifies pending HCP, cancels it, and confirms the first handler still
finishes while the second never runs. The test links the two emulator libraries,
not a firmware image or the MD device library.

The handler-return serializer, receive clamps/pre-command drain, physical
timing equivalence, complete reset/state restore, and illegal CVR writes while
HC is already set remain separate limitations. This change synchronizes
acceptance/cancellation; it is not a claim that the entire transport is fixed.

Companion commits: DSP `febeab64`, MCU `01b89c8`. Final Release validation:

- ARM64: 6/6 passed. Core 1.75 s, synthetic lifecycle 0.21 s, MD UW/RAM/modes
  37.19 s, MM boot/panel 13.41 s, sine/SRR/burst 44.70 s, full Ensemble sweep
  121.16 s.
- x86-64: 5/5 passed. Core 2.15 s, synthetic lifecycle 0.49 s, MM boot/panel
  20.47 s, sine/SRR/burst 66.80 s, full Ensemble sweep 180.33 s.
- ARM64 interpreter: core and synthetic lifecycle passed 2/2 (1.99/0.22 s).
  Firmware audio was not rerun in this increment; the known parity limitation
  remains. Legacy JIT scheduling was not rerun for this lifecycle increment.

The next transport experiment should test whether the handler-return wait and
extra-command serializer can be replaced by acceptance-based pending state now
that host HC is accurate. Do not infer this from the green matrix above: those
policies were retained throughout these runs.

## Acceptance-based admission versus handler-return tracking

The first experiment changed bridge waits and new-command dispatch to depend
on pending acceptance rather than handler return, while retaining the old
in-flight tracker for comparison. ARM64's synthetic, MD UW, MM boot, sine,
and Ensemble gates passed 5/5 (0.21/37.38/13.47/44.61/121.52 s). x86-64
synthetic/MM boot/sine/Ensemble passed 4/4 (0.58/20.79/66.94/199.43 s).

An experimental follow-up removed `pollHostCommandCompletion`, the saved stack
index, the handler-entry flag, and the atomic in-flight flag, making busy mean
pending acceptance everywhere. ARM64 firmware regressions passed, but x86-64
sine failed at track 1 / note 36 (roughness `0.00155457`, 35.33 s), while its
full Ensemble sweep passed. This stronger removal was therefore not retained.

The synthetic acceptance test now stops while the first handler is still in
long-interrupt mode, verifies that HCP has cleared, publishes a second
command, and requires a CPU request to be queued before the first handler
returns. Cancelling the second request must leave the first handler running.
This checks the mechanism, not merely the eventual audio result.

The queue assertion uses the read-only, owner-thread `DSP::hasQueuedInterrupts`
query. The older `hasPendingInterrupts` also counts a running handler and gave
a false positive here. Rebuilding with only the old return-gated admission
restored now fails with `second command waited for the first handler to return`.
Restoring acceptance-based admission is required to pass this regression.

The initial version of that test inspected a tiny handler after `DSP::exec()`;
JIT batches could already have completed it, invalidating the fixture. The
corrected fixture uses a 128-NOP handler and four-instruction JIT blocks to keep
it active across an execution batch. Its secondary handler stays within the
assembler's short absolute JSR range. Corrected synthetic tests pass on all
three execution configurations, including with the stronger removal candidate.

The remaining `hostCommandBusy()` runtime consumer is the empty-host-read
progress loop in `onUCRxEmpty`. Removing its interrupt/busy condition entirely
as a separate experiment produced immediate x86-64 sine silence on track 0;
unconditionally running to the existing clamp is not a supported replacement.
A temporary diagnostic on use of the command-overflow slot reported zero
events in the reproducibly failing stronger-removal sine run. Diagnostics and
the unconditional read loop were removed. These results narrow the investigation
toward read-side scheduling but do not establish the precise cause.

The existing one-entry overflow slot for writes while HC is already set is
retained as compatibility behavior, together with the return tracker needed by
the existing read-side progress policy. Neither is a completed hardware model.
The retained admission fix uses pending acceptance in `writeHostCommand` and
the renamed bridge helper `waitForHostCommandAcceptance`: a valid new command
can reach the CPU queue before the previous handler returns. It is not merely
a moved wait. The compatibility slot is not evidence of a hardware FIFO: valid host
software waits for HC to clear. Receive queue clamping, the MM pre-command
drain, full device reset/state restore, and physical timing remain separate
acceptance work. Final retained-candidate results follow below.

The retained runtime passed ARM64 6/6 (core 1.73 s, synthetic 0.21 s, MD UW
37.06 s, MM boot 13.56 s, sine 44.65 s, Ensemble 121.17 s) and x86-64 5/5
(core 2.17 s, synthetic 0.49 s, MM boot 20.38 s, sine 67.06 s, Ensemble
186.93 s). Interpreter core/synthetic passed 2/2 (1.93/0.21 s). These full
suites preceded the final queue-specific test assertion/read-only query;
final core/synthetic rechecks are recorded separately. No firmware runtime
behavior was changed by that observation-only API addition.

Final core/synthetic rechecks after the queue-specific assertion passed 2/2 in
each build: ARM64 2.75/0.24 s, x86-64 2.47/0.50 s, interpreter 1.78/0.22 s.
The temporary return-gated comparison and all diagnostics were removed before
these final checks. No interpreter firmware-audio parity claim is added.

## Remaining acceptance work

- Establish a baseline for each affected behavior and make hook activation
  observable in diagnostic builds. Keep diagnostics out of normal product UI.
- Separate panel-controller responsibilities from MCU execution, and hardware
  transport responsibilities from firmware-specific compatibility behavior.
- If temporary diagnostic switches are needed, make them explicit and local
  to this investigation; a switch that leaves the workaround enabled is not
  remediation completion.
- Preserve ROM fingerprint validation. Normal MD/MM loading already accepts
  only pinned supported images; the CPU hook lacks a separate check but is
  reached through that validated loader.
- Keep the recent MERGE, flag-preservation, DMA-wrapping, codec-routing, and
  mode-label fixes. Their correctness does not depend on retaining these hooks.

## Order and acceptance criteria

1. **Establish MM reproduction coverage.** Identify the supported MM fixture,
   capture the affected sine/nominal-rate case, DigiPRO output, and parameter
   changes. Confirm the current behavior first. Retain the existing MD boot,
   mode, RAM audio, and randomized scheduler tests as guards.
2. **Panel readiness and startup.** Validate startup and sustained LCD/LED
   updates using external interface events, with direct firmware task-list
   writes absent. Cover both models, first-run initialization, reset, and state
   restoration.
3. **Host transport and boot responses.** Check event ordering and progress
   without the parameter-memory hook or fabricated query response. Cover boot
   plus rapid parameter changes across voices, with bounded queues and no
   firmware-PC/private-variable conditions in the replacement.
4. **Waveform population.** Demonstrate DigiPRO bank availability and correct
   playback through the established hardware path without constructor injection.
5. **Audio correction.** Remove the CPU hook once nominal-rate and deliberately
   reduced-rate output are explained and validated. Check interpreter/JIT
   parity and bounded execution on ARM64/x86-64.

Each step should identify the relevant public hardware/protocol reference or
the permitted source of observations, add the narrowest useful regression,
show the replacement working with the old hook absent, and remove its obsolete
state and special cases. Do not replace one firmware address or task-layout
dependency with a different one.

The preferred implementation strategy is incremental behavior preservation.
An immediate removal strategy is materially different: it may disable boot,
panel operation, DigiPRO audio, or parameter updates. Select that tradeoff
explicitly before making dependent runtime changes.

## History and review boundaries

Most MD/MM cases were already present in the August 26 integration commit
`bd5800b8` (165 files, 14,477 insertions). Its DSP pointer was `329f5977`, which
already included the MM audio correction; corresponding later DSP history
contains `00c48833`. Waveform transfer behavior was refined in `635724ca` on
September 2. These hooks were not introduced by the recently merged RAM-audio
PRs #5/#42.

Repository history identifies integration points, not which person or model
originally devised a technique or what review/authorization occurred elsewhere.
The recent RAM-audio review covered its changed behavior; it was not a complete
firmware-provenance review of the pre-existing emulator.

Related notes:

- [RAM audio validation](md_ram_audio_validation.md)
- [RAM audio fix provenance and upstream candidates](md_ram_audio_fix_provenance.md)
