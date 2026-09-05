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
MD now publishes received HI08 words without reentrant status polling; MM keeps
the legacy polling path and its reproduced receive-word-loss defect remains open.
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

## Empty-read dependency and receive-status sampling

A follow-up isolated the read-side dependency without deleting or rearranging
the DSP return tracker. One-shot diagnostics on the passing x86-64 sine run
confirmed both empty host reads and iterations where the return tracker alone
keeps DSP progress running (no pending command and no CPU interrupt activity).
Changing only that loop predicate from `hostCommandBusy()` to
`hostCommandPending()` reproduced the same track 1 / note 36 sine failure,
roughness `0.00155457`. Thus the read-side policy is a demonstrated dependency,
not just a candidate inferred from the larger removal. The original predicate
was restored and all temporary diagnostics removed.

The status path also contained an independent sampling inconsistency:
`mc68k::Hdi08::isr()` snapshots its stored ISR before calling the MD bridge;
`Dsp::hdiUcReadIsr()` then advances the DSP and can fill the host receive latch,
but returned the earlier RXDF bit. A status read could therefore say empty
after that same callback had already latched a word. The bridge now refreshes
RXDF for MM from the stored latch status after catch-up, using a callback-free
helper that preserves all other input flags. MD retains its old sampling order
pending its timing regression below; other integrations are unchanged.

[DSP56303UM section 6.6.7](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
describes transfer into the host receive registers setting RXDF. This correction
makes the status returned by our bridge consistent with its current emulated
latch. It is not a model of the manual's separate peripheral-write/status
pipeline latency, nor proof of exact asynchronous hardware timing.

The synthetic regression delivers a word from inside the status callback,
including the reentrant observation during latching. It verifies RXDF on the
first outer read, unchanged three-byte data, cleared RXDF after consumption,
and preservation of unrelated status bits. The original return-based read
progress policy remains present while this sampling change is validated.

Initial global-refresh results: MM boot/sine/Ensemble passed ARM64 and x86-64,
and the synthetic test passed all three builds. However, MD UW failed its RAM
waveform oracle (correlation `0.724296`, wrong-input correlation `0.201194`,
RMS `0.0578798`, 36.60 s); a direct repeat reproduced identical measurements.
Pairing the global refresh with two receive stages for MD also failed: both
DSPs reported 1,338 codec input underflows and the RAM capture lost input-bus
synchronization. Neither MD experiment is retained. This is evidence of an
unresolved timing interaction, not proof that stale status is correct hardware.

Combining refreshed MM RXDF with pending-only empty-read progress got farther
than the earlier failure but still failed x86-64 sine: track 5 / note 36 RMS
`3.0823e-6`, reported as silence/non-finite audio. RXDF consistency alone does
not remove the return-tracker dependency. The retained change therefore
refreshes RXDF only for MM, keeps the existing empty-read predicate, and keeps
MD's one-stage pacing and old status sampling. No new firmware-PC, private
variable, payload signature, or arbitrary delay was added.

## Reentrant receive-latch overwrite and INIT follow-up

A standalone firmware-free reproducer found an additional receive-path bug:
`pollRx()` removes the first word and assigns `m_rxd`, then invokes the status
callback before publishing RXDF. If the callback delivers a second word,
`writeRx()` sees an empty latch and recursively replaces `m_rxd`. The first
host byte is consequently `0x44` instead of `0x11` for the synthetic words
`0x112233`, `0x445566`. This is actual word loss, not merely stale status.

The manual target `mdHdi08ReentrancyRepro` preserves that small reproducer:

```sh
# From the repository root, reconfigure the existing review build first.
cmake -S . -B /private/tmp/md-review-build
cmake --build /private/tmp/md-review-build --target mdHdi08ReentrancyRepro
/private/tmp/md-review-build/source/elektron/md/mdLibTest/mdHdi08ReentrancyRepro
```

It intentionally reports the unresolved bug with exit 1. It is an investigative
executable, not registered as a passing CTest acceptance gate. This known
failure must not be omitted from the branch's completion/merge audit.

Publishing the occupied receive latch before invoking status callbacks fixed
the synthetic overwrite and preserved MD UW (36.77 s), but broke MM startup
on ARM64 and x86-64: MM boot and both audio gates failed their startup checks.
The early-publication experiment was reverted. The accepted MM-only RXDF
refresh does not fix this underlying overwrite.

A related public-hardware mismatch is the bridge's INIT callback, which clears
INIT and reports TX-ready without implementing the directional initialization
matrix. [DSP56303UM table 6-15](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
specifies:

| TREQ | RREQ | Additional effects when INIT executes |
| --- | --- | --- |
| 0 | 0 | None beyond clearing INIT |
| 0 | 1 | RXDF cleared, HTDE set |
| 1 | 0 | TXDE set, HRDF cleared |
| 1 | 1 | Both directions initialized as above |

INIT always clears after execution. Directional initialization and reentrant
latch publication should be investigated together, with independent register
tests and the same firmware gates. That is a next hypothesis, not an established
explanation for the MM startup dependency. No firmware payload/signature should
be used to choose which word to discard or synthesize.

Retained RXDF-refresh validation (MCU helper commit `aec3524`):

- ARM64: 5/5 passed — synthetic 0.32 s, MD UW 37.32 s, MM boot 13.40 s,
  sine 44.60 s, Ensemble 121.06 s.
- x86-64: 4/4 passed — synthetic 0.50 s, MM boot 20.95 s, sine 68.34 s,
  Ensemble 179.92 s.
- Interpreter: synthetic status/lifecycle coverage passed 1/1 (0.26 s).
  Firmware interpreter audio was not rerun and remains unresolved.
- The separately built manual overwrite reproducer still exits 1 with first
  byte `0x44`; this is preserved failure evidence, not a passing test.

## Receive-request coalescing audit

The INIT audit also exposed an unsupported request condition in
`Hardware::pumpDsp2HostRequest`: HREQ waits for three host-receive words, rather
than following an occupied receive latch. `git blame` traces the threshold and
its detailed MAME attribution to `bd5800b8` (2026-08-26), like the earlier
integration hooks. That attribution is unverified by the public driver check
above and has been relabeled in the source as retained compatibility behavior.
This identifies the integration point, not the original author or derivation.

[DSP56303UM table 6-15](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
specifies that RREQ enables a receive request when RXDF is set. It does not
describe waiting for a reply-sized three-word batch. This audit tested only
that receive-request component; TREQ/HDRQ routing remains separate work.

ARM64 MM boot comparison (all changes temporary):

| Receive request | Existing reentrant latching | Publish occupied latch before callbacks |
| --- | --- | --- |
| Existing three-word threshold | Passing retained baseline | Startup fails (previous increment) |
| RREQ and occupied latch/RXDF | Pass, 13.92 s | Startup fails, 11.51 s |

Thus fixing the receive-request condition does not by itself make the
word-preserving publication change compatible with MM startup. With existing
latching and only the RXDF request change, broader firmware tests failed:
ARM64 MD UW (33.15 s), sine, and Ensemble; x86-64 sine and Ensemble. MM boot
still passed on both architectures. The request experiment and the early
publication experiment were reverted; the three-word threshold remains an
explicit unresolved compatibility rule, not a completed hardware replacement.

INIT's directional flag matrix remains a next investigation, not an implemented
fix or established cause of these failures. No firmware packet length, private
variable, or payload signature should be added to explain away this mismatch.

After restoring all experimental runtime changes, ARM64 synthetic/MD UW/MM boot
passed 3/3 (0.23/37.56/13.47 s), and x86-64 synthetic/MM boot/sine passed 3/3
(0.50/20.43/65.99 s). The retained source changes in this audit are provenance
comments only; earlier passing Ensemble coverage still applies to the unchanged
runtime. Neither the request threshold nor receive-word overwrite is resolved.

### INIT occurs with occupied receive queues

A temporary callback-entry diagnostic recorded only the DSP index, ICR's two
request-enable bits, and queue counts, before the existing INIT callback changed
anything. No payloads, firmware PCs, or private memory were inspected. At
`d7467ac4`, the observations were:

| Fixture/build | DSP index | ICR & 3 | Host receive words (including latch) | DSP transmit words | DSP receive words |
| --- | --- | --- | --- | --- | --- |
| MM boot, ARM64 JIT | 1 | 1 | 17 | 8192 | 0 |
| MM boot, x86-64 JIT | 1 | 1 | 17 | 8192 | 0 |
| MD UW, ARM64 JIT | 0 | 1 | 0 | 0 | 0 |
| MD UW, ARM64 JIT | 1 | 1 | 17 | 1 | 0 |

The MD pair occurred three times during the UW test. Both MM boot tests passed
(13.11/21.09 s); MD UW passed (37.28 s). These are observations of the retained
model, not measurements of physical hardware or proof that queued data is valid.

ICR & 3 == 1 selects receive initialization (RREQ=1, TREQ=0).
[DSP56303UM table 6-15](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
specifies clearing INIT and RXDF and setting HTDE for that combination. The
current bridge instead clears INIT and sets host TXDE/TRDY irrespective of the
direction. Its replacement cannot be dismissed as an empty-queue flag update:
the relevant queues are occupied in both models, and MM's DSP queue is full.

The host count is a latch plus a 16-word backing queue. MM additionally enables
an 8192-word DSP transmit ring; MD leaves transmit buffering disabled. Source
inspection shows that `HDI08::writeTX` replaces the ring's front word when the
buffered ring is full (or the unbuffered transmitter is already occupied).
Fullness alone does **not** establish that another write occurred or quantify
loss in these runs. It does establish that the existing comment asserting
scheduler backpressure must not be read as a guarantee against saturation.

No queue-clearing experiment or directional INIT fix was retained in this
increment. Clearing all software queues would discard substantially more state
than resetting physical transfer registers, whereas clearing RXDF alone can
immediately expose queued data again. A coherent replacement needs an explicit
register/transfer-state model, independently tested INIT combinations and
partially consumed host words, and firmware regressions covering the observed
occupied startup path. This observation does not establish INIT as the cause of
the earlier receive-latching or receive-request failures. The temporary
diagnostic was removed; retained changes are documentation/comments only.

### Combined receive reset, latch publication, and request experiment

The next experiment tested whether pending pre-INIT receive data explains why
publishing RXDF before a reentrant status callback previously broke MM startup.
The temporary receive-only addition, conditional on RREQ, drained the DSP TX
software ring without executing the DSP, cleared the host receive queue/latch,
and set DSP HTDE before the legacy INIT callback completed. It left the existing
TXDE/TRDY behavior unchanged. This was explicitly a **whole-software-queue reset
hypothesis**, not a complete implementation of table 6-15 or a claim that
discarding 8192 software words matches resetting physical registers.

| Temporary configuration | MM boot | MM sine | MM Ensemble |
| --- | --- | --- | --- |
| Receive reset + early RXDF publication, legacy three-word request | ARM64 pass 13.93 s; x86-64 pass 22.72 s | Both fail: track 0 silence (17.31/26.65 s) | Both fail: track 0 silence (17.71/27.10 s) |
| Same + RREQ && RXDF request | ARM64 pass 14.44 s | ARM64 fails smoothness/pitch (17.27 s), RMS 0.0212008, roughness 0.0113837 | Not run |
| Receive reset alone, legacy publication and request | Not separately run | x86-64 pass 65.72 s | Not run |

The first configuration also passed ARM64 MD UW (37.53 s) and the ARM64/x86-64
synthetic host-interface gates (0.24/0.56 s). Early publication now passes the
expanded firmware-free receive reproducer: both complete words are delivered in
order in both host byte orders, and no receive word remains pending afterwards.
Without early publication, the retained model still fails on its first word.
The reproducer remains a manual known-failure executable, not a passing CTest
gate; its checks were strengthened in this increment.

These results narrow, but do not resolve, the interaction. Clearing pre-INIT
receive state makes the word-preserving latch change compatible with the MM
boot gate; the x86-64 reset-only control passes sine, so the combined silence
cannot be attributed to that reset addition alone. Replacing request coalescing
recovers nonzero audio in the combined case, but not correct sine output.
Neither boot success nor nonzero audio is sufficient evidence of a replacement.
The next transport investigation must account for the remaining waveform
failure while preserving every receive word, rather than relying on the
existing overwrite. No firmware payload, PC, private address, or algorithm was
used to select these experiments. All experimental runtime changes were
reverted; no INIT or reentrancy fix is claimed by this increment.

After restoration, ARM64 synthetic host-interface and MM sine passed 2/2
(0.30/44.06 s). The strengthened manual reproducer returned exit 1 and reported
`0x445566` as the first word, confirming that the known overwrite is still
present rather than accidentally marked resolved.

### Callback-free receive publication: MD rollout

The next experiment removed status-callback invocation from `pollRx()` entirely,
using the stored ISR bits while publishing an already-arrived word. It fixes
the synthetic receive-word loss without introducing a FIFO flush, delay, or
firmware-specific condition. Unlike early RXDF publication followed by a status
callback, this also prevents peer execution during latch publication.

Enabled for both models temporarily, it passed the two-word/both-byte-order
reproducer but failed ARM64 MM boot (11.68 s) and sine startup (11.78 s). Adding
the earlier experimental receive reset and RREQ && RXDF request passed MM boot
(14.25 s), but sine still failed its smoothness/pitch gate (17.24 s). Thus peer
execution during latch publication is not the sole cause of the combined
configuration's waveform failure. Those MM, INIT, and request changes were
reverted. Callback-free publication alone passed ARM64 MD UW (36.42 s).

The retained incremental fix is explicit and limited to MD:

- `Hdi08::setReceiveLatchStatusPolling(false)` publishes data using the stored
  register flags, without executing status callbacks in the publication window.
  It is configured before transfers. This is synchronous callback exclusion,
  not a new claim of thread safety or an atomic-memory implementation.
- MD disables that polling on both host ports. MM retains the legacy default;
  other products' configuration and behavior are unchanged. This rollout split
  is an unresolved emulator compatibility constraint, not a claimed difference
  between the physical HI08 devices.
- The manual reproducer accepts `--no-latch-status-poll`, checks that initial
  publication invokes no status callback, then verifies both complete words in
  order for each byte order and an empty latch/queue after consumption. That
  mode is registered as `mdHdi08ReceiveLatchTest`. No-argument mode retains the
  legacy configuration and must still be reported as a known failing check.

This removes the independently reproduced overwrite from MD's publication
path. It does not complete MM transport remediation, validate the three-word
request threshold or extra FIFO capacity, implement directional INIT, or
remove the MD private task-list workaround. The opt-in exists to preserve
behavior during migration, not to declare the default path correct.

MCU implementation commit: `62dc26c`. Retained-configuration validation:

| Build | Passing checks |
| --- | --- |
| ARM64 JIT | Host hardware 0.27 s; receive latch 0.13 s; MD UW 37.43 s; MM boot 13.73 s |
| x86-64 JIT | Host hardware 0.56 s; receive latch 0.33 s; MD UW 56.87 s; MM sine 66.03 s |
| ARM64 interpreter | Host hardware 0.21 s; receive latch 0.11 s |

The no-argument legacy reproducer still exits 1 with first word `0x445566`.
Interpreter firmware audio was not rerun; its parity problem remains open.
An additional temporary removal of the MD task-list-post caller, with the MD
receive fix enabled, still failed first-run UW flash initialization (9.89 s).
That caller was restored exactly; the passing retained-configuration runs above
include it. Thus this receive fix does not justify removing the panel workaround.

### Panel/UART audit: status aliasing and a missed receive request

Reviewing the external panel path found two independently reproducible defects
in the MD/MM SIM UART model, without reading private firmware state:

1. `read8(base + g_uartIsr)` fell through to the stored UIMR write value.
   [MCF5206EUM sections 12.4.1.10/.11](https://www.nxp.com/docs/en/data-sheet/MCF5206EUM.pdf)
   distinguish source status (UISR read) from interrupt enables (UIMR write).
   Masking a source does not hide it from a UISR read. The new status helper
   reports the transmit/receive-ready sources already represented by the model,
   independently of the stored mask.
2. `queueRx()` armed receive service only if UIMR already enabled it. A byte
   arriving while masked could remain stranded after enabling RX: UIMR writes
   previously armed only TX. The receive-ready source is now armed on an
   enabling mask write when data is already queued.

The firmware-free `mdUartRegisterTest` covers both ports, masked status reads,
mask changes without source/data changes, FIFO consumption without mask
changes, and enabling a pending receive source with TX disabled. The first
version failed with "masked receive readiness disappeared from UISR"; after
the alias fix, the added unmask case failed with "unmasking stranded an
already-received byte". Both defects thus have red-before-fix evidence.

These are corrections to our SIM integration, not firmware-specific protocol
inferences. `git blame` traces the stored-value read fallback and TX-only
UIMR arming to the initial `bd5800b8` integration; this identifies where they
entered this repository, not who originally devised them. Neither correction
changes panel reply bytes or introduces a private-memory/PC condition.

The helper deliberately reports only existing modeled sources. UART
enable/reset commands, mode-register pointers, receive-full interrupt
selection, serial timing/physical FIFO capacity, delta-break/CTS events, and
fully level-sensitive delivery remain incomplete. This is not a claim of a
complete UART model or proof that the missing panel notification is resolved.

With both fixes retained, ARM64 passed UART registers (0.18 s), MD UW (37.80 s),
and MM boot (13.54 s); x86-64 passed UART registers (0.31 s), MD UW (57.26 s),
and MM sine (67.12 s). The final additional no-extra-request assertion passed
on ARM64/x86-64 (0.19/0.32 s). The alias-only intermediate configuration also passed ARM64
UART/MD UW/MM boot (0.18/36.27/13.15 s).

A temporary removal of `panelDisplayReadyPost()` with both UART fixes enabled
still failed first-run UW initialization (9.97 s). The caller was restored
exactly. These fixes are retained for their independent register/interrupt
correctness, not presented as a completed panel replacement. No panel
handshake byte or private task-list operation was changed.

## Current interpreter audio and DigiPRO revalidation

Rebuilt the firmware gates at `d69a78a3`, after the receive-latch and UART
corrections. The interpreter build cache confirms `DSP56K_FORCE_INTERPRETER=ON`.
Its UART register test passed (0.23 s), and MM boot/MIDI/repeated panel-menu
checks passed (46.58 s), but the sine test failed its initial idle-audio gate
(48.17 s). A repeat with a newly printed idle RMS value failed again (48.10 s):
`2.51706e-6` versus the unchanged `1e-7` limit. This is low-level residual audio,
not evidence by itself of loud corruption or a CPU crash.

To determine whether that threshold was hiding otherwise correct tones, a
temporary diagnostic executable continued past only the idle assertion. It
failed the subsequent track-0 sine smoothness/pitch check (57.93 s). Comparison
with the retained ARM64 JIT sine run:

| Measurement | ARM64 JIT | Interpreter diagnostic |
| --- | --- | --- |
| Idle RMS | 0 | 2.51706e-6 |
| Track 0 note-60 RMS | 0.113775 | 0.104885 |
| Track 0 zero-level RMS | 0 | 4.47202e-7 |
| Track 0 roughness | 1.92966e-6 | 0.0600631 |

Thus the discrepancy is not solely the strict idle threshold. These external
audio measurements do not identify an instruction defect, prove the selected
machine's identity, or distinguish instruction execution from transport/panel
setup as the cause. No firmware PC, private memory, or algorithm was inspected
to obtain them. The idle assertion was restored in source; the only retained
test change prints its measured RMS. ARM64 JIT still passes the full sine gate
(44.71 s). Do not label the relaxed diagnostic as a passing acceptance run.

After rebuilding the interpreter executable with the strict assertion restored,
the gate failed as expected (46.48 s), with the same idle RMS. Neither source
nor the final interpreter binary retains the diagnostic bypass.

The extended hook-free DigiPRO gates were also rerun on the current source:

| Gate | ARM64 JIT | x86-64 JIT |
| --- | --- | --- |
| DDRW, full selector sweeps across six tracks | Pass 124.56 s | Pass 187.38 s |
| DENS, full selector sweeps across six tracks | Pass 122.47 s | Pass 182.79 s |

These renew behavioral regression evidence after the UART corrections; they
do not establish exact physical waveform identity or resolve interpreter
parity. A useful next discriminator is independently verifying the selected
machine/setup through documented external controls before attributing the
interpreter's waveform discrepancy to a particular instruction implementation.

### Explicit GND-SIN assignment fixture

Added `mmSineMidiFirmwareTest` (`mmAudioFirmwareTest --sine-midi`) as a separate
fixture; the original panel-clear sine gate remains unchanged. After the
ordinary empty-kit setup, this variant requests GND-SIN on all six tracks using
the manufacturer's [Appendix C SysEx assignment command](https://www.bhphotovideo.com/lit_files/85386.pdf):
command `0x5b`, machine 1, initialize all data pages. It uses the same one-second
per-track processing interval as the existing DigiPRO assignment fixture and
retains the same idle, level, pitch, SRR, and burst-parameter assertions.

| Build/configuration | Result |
| --- | --- |
| ARM64 JIT, strict gate | Pass 48.86 s; idle RMS 0; track-0 RMS 0.113738, roughness 1.93019e-6 |
| x86-64 JIT, strict gate | Pass 74.53 s |
| Interpreter, strict gate | Fail idle check 60.73 s; RMS 2.88764e-7 |
| Interpreter, temporary idle-assertion bypass only | Fail track-0 smoothness/pitch 69.64 s; RMS 0.105696, zero-level RMS 2.74138e-7, roughness 0.0594902 |

The documented assignment path does not restore interpreter audio parity. It
also adds initialization and elapsed emulation time, so the lower idle RMS is
not evidence that a particular cause was isolated. Acceptance by `sendMidi`
proves enqueueing, not successful application of the requested machine; no
private kit layout was inspected for readback. The retained fixture strengthens
external-command coverage but does not rule out transport/setup failure or
identify a faulty instruction.

The temporary assertion bypass was removed and the interpreter executable
rebuilt from strict source. It is not an accepted runtime or test change. The
only retained changes are the additional documented setup/test path and this
record; no firmware hook was added or re-enabled.

### UART request retention across CPU masking

Source review raised a narrower question than full UART level-sensitive
behavior: does consuming a SIM readiness event before CPU acknowledgement lose
it when the CPU's SR mask prevents immediate service? The CPU wrapper retains
an offered vector in its pending queue; a synthetic test now verifies that
path rather than assuming event consumption means request loss.

`mdUartCpuInterruptTest` connects the real SIM request interface to a synthetic
MCF5206E CPU. For each UART it queues one byte, offers exactly one receive
interrupt, executes while the CPU is masked, and verifies the request remains
pending. It then unmasks the CPU without adding another UART event and verifies
that a small handler sets D0, acknowledgement removes the pending vector, and
the UART byte remains unread. Main and handler instructions, reset vectors,
stack, and data are entirely synthetic; no firmware is loaded.

The fixture compiles the standalone SIM with the MCU core and supplies base
memory callbacks. Its initial attempt incorrectly linked mdLib's specialized
callbacks, which require an actual `md::Microcontroller`, and omitted physical
reset vectors. Those fixture errors were corrected; they are not emulator
failure evidence. The passing test rules out CPU masking alone as the proposed
loss mechanism for an already-offered UART request. It does not cover source
withdrawal before acknowledgement, persistent level re-offering, serial
timing, or panel protocol readiness. No production interrupt handling changed,
and the private panel workaround remains unresolved.

Current validation is ARM64 only (pass, 0.15 s). The x86-64 process did not
return within its configured timeout and remained in OS state `U` after a
stop request. No x86-64 pass or emulator defect is inferred from that unresolved
process state. The new fixture and this audit remain pending validation.

A same-host control also stalled: the already-built x86-64
`mdUartRegisterTest`, previously passing, entered uninterruptible state, while
its ARM64 counterpart completed successfully. A bounded sampling attempt
produced no report and was stopped. This is evidence of a broader x86-64
execution/host problem, not an isolated failure of the new interrupt fixture;
its OS-level cause is not established. Stop requests were issued for the stuck
diagnostic children. No host services were restarted and no more x86-64 copies
were launched. Validation remains pending restoration of host execution.

Follow-up after the user dismissed a host dialog: both previously stopped
processes exited as killed, not as passing tests. Fresh runs now pass:
`mdUartCpuInterruptTest` on ARM64 (0.00 s reported by CTest), and both
`mdUartRegisterTest` (0.72 s) and `mdUartCpuInterruptTest` (0.28 s) on x86-64.
The new fixture is therefore validated on both architectures. The earlier
host-level stall's cause remains unidentified; these results do not establish
complete UART interrupt-model correctness or resolve panel readiness.

## Single-latch MM transport experiments after goal resumption

The expanded goal checklist was published in `10869a94`. Investigation then
returned to MM receive-word loss rather than treating the MD-only rollout as
complete. A source audit found that the shared MCU `Hdi08::exec()` contains
an old 50-cycle receive timeout, but MD/MM `advanceAfterCpu()` advances SIM,
not that HI08 execution method. No timeout change was made to unrelated users.

[DSP56303UM sections 6.3/6.4 and table 6-15](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
describe the physical double-buffered data path and receive requests from
RREQ/RXDF. A temporary MM-only experiment disabled the 8192-word DSP transmit
backlog, bypassed the 16-word host backing queue, used callback-free host-latch
publication, and requested receive service from an occupied latch instead of
three queued words. MD's retained behavior was unchanged. This is only a
partial model experiment, not complete hardware timing or INIT emulation.

ARM64 JIT results with strict assertions unchanged:

| Configuration | Result |
| --- | --- |
| Single host latch, unbuffered DSP transmit, RXDF request | MM boot/MIDI/panel gate passes 15.87 s; sine fails 18.88 s, track 0 RMS 4.97016e-8 (silence), idle RMS 0 |
| Same, but restore buffered DSP transmit | Sine fails startup, 12.78 s |
| Unbuffered configuration plus receive-side INIT clearing host latch and DSP transmit data | Sine fails 18.43 s with the same track 0 RMS 4.97016e-8 and idle RMS 0 |
| Same plus immediate DSP-write-to-empty-host-latch transfer, with no recursive CPU execution | Sine fails smoothness/pitch at 18.75 s; track 0 RMS 0.0216794, roughness 0.0103479, idle RMS 0 |

Thus reducing the software queues permits startup in a configuration that
preserves host-latch words, but does not preserve audio. Restoring buffering
alone does not repair that configuration, and clearing pre-INIT receive data
alone does not explain its silence. No firmware payload or private state was
used to choose these experiments. Logs are local `/private/tmp/mm-single-latch-*`;
the results above, not an untracked log's continued existence, are the durable
record. These results do not identify the remaining defect or justify accepting
the replacement. Immediate latch transfer recovers nonzero audio, showing
sensitivity to transfer timing, but still fails quality. A next useful check is
to count transmit writes, transfers and occupied-register replacements across
startup versus note playback, using peripheral counters rather than firmware
contents. All temporary runtime changes in this experiment were reverted.
After rebuilding the restored ARM64 binaries, the strict MM sine gate passed
(47.94 s) and callback-free MD receive-latch regression passed (0.04 s).
No new runtime fix is claimed by this increment; x86-64/interpreter variants
of these failing experimental configurations were not run.

## Measured DSP transmit replacements

Following `140c5cda`, temporary owner-thread counters were added at
`HDI08::writeTX` entry, its occupied/full-register replacement branch, and
`readTX` after the nonempty wait. The MM audio harness reported cumulative
counts after 20 seconds of emulated startup, after empty-kit setup/idle render,
and at each track's note render. No payload, firmware address, PC, or private
state was logged. Queue depth was read without polling the peripheral.

ARM64 results for DSP2 (index 1):

| Configuration / phase | Writes | Reads | Replacements | Still queued |
| --- | ---: | ---: | ---: | ---: |
| Retained JIT / boot | 322860 | 295640 | 27220 | 0 |
| Retained JIT / idle | 378658 | 351438 | 27220 | 0 |
| Retained JIT / first note | 465423 | 438203 | 27220 | 0 |
| Retained JIT / sixth note | 1389849 | 1362629 | 27220 | 0 |
| Retained interpreter / boot | 322850 | 295630 | 27220 | 0 |
| Retained interpreter / idle | 378640 | 351420 | 27220 | 0 |
| Single latch + immediate transfer + receive INIT / boot | 349990 | 314490 | 35500 | 0 |
| Same experiment / idle | 410080 | 374580 | 35500 | 0 |
| Same experiment / first note | 503521 | 468021 | 35500 | 0 |

Every reported snapshot satisfies writes = reads + replacements + queued.
DSP1 (index 0) recorded zero replacements throughout these runs. Read counts
in the INIT experiment include words removed by its temporary reset drain;
they must not be interpreted as words consumed by ColdFire firmware.

The retained JIT completed the full strict sine test (exit 0), first-note RMS
0.113775 and roughness 1.92966e-6. Interpreter failed the unchanged idle gate
(exit 1), RMS 2.51706e-6. The single-latch experiment reproduced its strict
first-note failure (exit 1), RMS 0.0216794 and roughness 0.0103479.

This establishes actual transmit replacement before the first post-boot
snapshot, beyond the earlier evidence of queue saturation alone. Cumulative
replacement counts did not increase between the observed boot and later
snapshots. It does not locate the writes relative to INIT, establish that the
discarded data was needed, or prove a physical transmitter would see the same
writes/timing. Equal replacement counts do not imply identical payloads,
delivery timing, or receive-latch behavior, and therefore do not rule transport
out as a contributor to interpreter failure. The callback-driven host-latch
overwrite is separate and is not counted by these DSP-side counters.

Next useful measurement: split the startup count at peripheral INIT and track
host-latch publications/consumption, without reading payloads. This can separate
pre-initialization replacement from loss during active host communication.
All temporary diagnostic and runtime edits were removed after measurement;
no new runtime fix is claimed. Local logs: `/private/tmp/mm-tx-counters-*.log`.

## INIT boundary and host-latch loss measurements

A subsequent retained ARM64 JIT sine run (`fb8a4877` plus temporary counters)
counted MCU-side `writeRx` arrivals, last-byte `readRX` pops, and `pollRx`
publications made with `m_pollRxDepth != 0`. The latter identifies publication
inside an existing latch's status callback, overwriting its not-yet-published
word. The counters were cumulative and did not read payloads or firmware state.
The INIT callback reported them before executing its existing reset behavior.

DSP2 measurements:

| Phase | Host arrivals | Host pops | Nested publications | Pending host words |
| --- | ---: | ---: | ---: | ---: |
| Receive INIT | 164 | 146 | 1 | 17 |
| After 20-second startup | 295640 | 218968 | 76672 | 0 |
| After setup / idle | 351438 | 261888 | 89549 | 1 |
| First note | 438203 | 328642 | 109561 | 0 |
| Sixth note | 1362629 | 1039893 | 322736 | 0 |

Each snapshot satisfies arrivals = pops + nested publications + pending.
DSP1 recorded zero nested publications and equal arrival/pop counts throughout.
At the single observed DSP2 receive INIT, the DSP transmit replacement count
was already 27220: all replacements counted in the previous retained run had
already accumulated at that boundary. This is separate from host-latch loss,
which continues to accumulate after INIT, through startup and note playback.

The strict six-track sine test still passed (exit 0, idle RMS 0). Thus passing
audio does not validate this retained transport: 322736 arriving host words
have been lost to nested publication by the sixth note snapshot. These counts
measure this emulator configuration; they do not establish what the physical
firmware would transmit, whether any particular lost word is required, or which
downstream state explains sensitivity to the word-preserving replacement.

Next investigation should compare scheduler/DSP progress and physical peripheral
transfer timing between retained and word-preserving configurations. The prior
single-latch run roughly doubled DSP1 host-write counts over the same requested
startup interval, so a change in effective execution/progress is a candidate
to test, not a proven cause. No private firmware interpretation is needed for
cycle, frame, or peripheral-transfer counters. All temporary measurement edits
were removed. Local evidence: `/private/tmp/mm-init-host-counts.log`.

## Scheduler/frame comparison

Following `aa7aa663`, temporary read-only snapshots recorded requested machine
frames, executed UC cycles, both DSP scheduler frame positions (cycle counts
relative to their latched boot origins), and generated codec-frame count. They
did not execute CPUs or inspect firmware state. ARM64 results:

| Configuration / phase | Requested frames | UC cycles | DSP1 frame position | DSP2 frame position | Codec frames |
| --- | ---: | ---: | ---: | ---: | ---: |
| Retained JIT / boot | 882000 | 800000000 | 882000.001408 | 882000.004196 | 813169 |
| Retained JIT / idle | 1019352 | 924582320 | 1019352.005748 | 1019352.002894 | 950521 |
| Retained JIT / first note | 1232930 | 1118303857 | 1232930.001408 | 1232930.001592 | 1164099 |
| Single latch + immediate transfer + receive INIT / boot | 882000 | 800000000 | 882000.000540 | 882000.000290 | 813169 |
| Same experiment / idle | 1019352 | 924582314 | 1019352.009220 | 1019352.003328 | 950521 |
| Same experiment / first note | 1232930 | 1118303856 | 1232930.000540 | 1232930.010272 | 1164099 |
| Retained interpreter / boot | 882000 | 800000003 | 882000.000106 | 882000.000724 | 813169 |
| Retained interpreter / idle | 1019352 | 924582319 | 1019352.000974 | 1019352.002026 | 950521 |

The retained JIT again passed strict sine. The single-latch experiment again
failed first-note quality (RMS 0.0216794, roughness 0.0103479); interpreter
again failed idle RMS (2.51706e-6). Generated frame counts match exactly at
common checkpoints, and CPU progress is close to the same requested target.
Thus the previously observed increase in host writes does not establish gross
DSP over-execution or codec-output starvation. These coarse snapshots do not
exclude transient interleave, sub-frame timing, stale/incorrect sample contents,
or instruction-emulation errors. Finer peripheral ordering remains open.

Source audit also found stale clock comments: the actual fixed UC rate is
40000000 Hz, giving about 907.03 cycles per codec frame, not the commented
25.447 MHz / 577-cycle ratio. Comments were corrected without changing timing.
The 40 MHz constant and contradictory comments were both already present in
the initial `bd5800b8` integration, not introduced by this remediation branch.
Physical-board clock provenance and PLL behavior remain unverified. The
scheduler's claim of a lossless host stream and unsupported detailed MAME queue
attribution were also corrected in light of measured overwrite; its runtime
threshold/clamp were not changed.

All temporary snapshot and experimental runtime changes were removed. Local
logs: `/private/tmp/mm-progress-{jit,single,interpreter}.log`. No new runtime
fix is claimed by this measurement.

## Firmware-free arithmetic comparison and scale-up extension flag

A new manual `mdDspArithmeticParityTest` diagnostic assembles synthetic single
instructions and compares interpreter/JIT A/B, X/Y, SR and PC across deterministic
56-bit/48-bit input samples and the three defined scaling modes. It needs no
ROM. It explicitly selects the specialized JIT mode after direct synthetic SR
setup. Neither backend is an ISA oracle; mismatches require independent
specification checks, including which flags have defined results.

The first apparent ABS/U mismatch disappeared after correcting that fixture's
JIT-mode setup. It was not evidence of an ABS implementation defect. A shared
regression's first failed comparison also required correction: `sr_test` returns
a mask, so its value must be converted to bool before comparison with an oracle.
Neither fixture error is counted as an emulator bug.

The corrected differential diagnostic exposed scale-up E-flag mismatches for
SUB, MACR and RND results near the signed 56-bit limits. The interpreter formed
its integer-portion mask by shifting `0x3fe` right in scale-up mode, obtaining
`0x1ff` and dropping accumulator bit 55. The required scale-up portion includes
bits 55 through 46, not just 54 through 46.
[DSP56300FM table 5-1](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
defines the signed integer portion for each scaling mode. The corrected mask
keeps the sign bit in all modes: normal `0x3fe`, down `0x3fc`, up `0x3ff`.
DSP implementation and shared regression commit: `9223c5a2`.

The shared `conditionCodes` regression now checks both accumulators, 13 boundary
values, and all three scaling modes with independently computed expected E
(compare each integer-portion bit to bit 55). ARM64 full core tests pass after
the fix (2.00 s), as do x86-64 full core tests (3.19 s). The differential E
mismatches disappear on both architectures, while separate NEG,
ASL and ADDL flag discrepancies remain to be classified; the diagnostic exits
1 and is deliberately not registered as a passing CTest gate. These random
samples do not cover every boundary or saturation/arithmetic mode.
The x86-64 diagnostic also finds an ASR flag difference. Its pseudo-random
stream currently advances until each instruction's first mismatch, so later
instruction inputs can differ between architectures when earlier failures
differ. Each failure prints its complete input; comparisons must use that
input rather than assuming a matching trial number means identical state.

The strict interpreter MM sine run with the mask fix still fails idle RMS at
2.51706e-6. No link from this isolated core defect to the MM audio failure has
been established, and no relaxed audio assertions are retained. A short disk
space interruption prevented two diagnostic launches; those were not test
results. Two completed interpreter logs were compressed to `.log.gz` without
losing their contents; pending source files were checked intact afterwards.

## Panel evidence intake

A further public-source check found no independent specification for the local
startup replies or display-readiness notification. The manufacturer's service
article supplies no such interface description; the current MAME driver remains
a skeleton. Search results do not prove that a source is unavailable elsewhere.
The [panel evidence checklist](md_mm_panel_evidence_requirements.md) records
what a usable source or authorized physical-device observation must establish,
and the acceptance gates for removing the private task-list write. Additional
generic peripheral fixes do not by themselves settle this provenance question.

## Remaining acceptance work

The [expanded goal checklist](md_mm_remediation_goal_checklist.md) records all
remaining scope requested for unattended work, including broader validation and
PR organization. Use both documents for the final completion audit.

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

## x86-64 ASR carry regression (September 5 follow-up)

The firmware-free arithmetic investigation confirmed an x86-64 JIT ASR carry
defect. [DSP56300FM, ASR, section 13](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
requires C to contain the last discarded accumulator bit, or zero for a zero
shift count. The JIT extends/aligns the accumulator into host bits 63:8;
after SAR, the relevant bit is host bit 7. The old code instead read host CF
after the restore helper's AND had already cleared it. The fix copies bit 7
before restoring the accumulator, matching the existing ARM64 strategy.

Shared synthetic tests use an independent unsigned 56-bit sign-fill oracle:
four positive/negative/boundary patterns, counts 0/1/7/8/16/24/55, immediate
and X0 counts, and A-to-A/A-to-B destinations (112 cases per backend). They
check result, C, cleared V, and source preservation for A-to-B. These cases
cover normal 56-bit mode, not saturation or sixteen-bit arithmetic modes.
The unchanged x86-64 JIT failed the new C assertion; the fixed full core
suite passed in 2.61 seconds, including interpreter tests. The ARM64 full core
suite also passed in 2.17 seconds. No new MM audio pass is claimed.
DSP fix commit: `4b4a2e22`. The rebuilt x86-64 differential diagnostic reports
ASR parity with the per-instruction seed reset; it still exits 1 for NEG, ASL,
and ADDL flag mismatches. Those are investigation leads, not additional proven
fixes or a passing overall parity gate.

Blame identifies `479407a4` (2026-08-19, left-aligned ALU extend/restore
helpers) for the current alignment/restore path, and older `402a280c` for
the retained host-carry read. This is not a firmware-derived algorithm.
Separate diagnostic cleanup resets the random seed per instruction and prints
trial numbers in decimal, making cross-architecture samples reproducible even
when preceding instructions fail at different trials. Other arithmetic flag
discrepancies and all unchecked goal acceptance items remain open.

## NEG overflow classification (September 5 follow-up)

The next differential mismatch is independently supported by
[DSP56300FM NEG, page 13-144, and table 5-1](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf):
NEG performs 56-bit two's-complement negation, preserves C, updates V, and
latches overflow into L. In normal arithmetic mode only the minimum signed
56-bit input overflows. The interpreter retained stale V (and could therefore
latch stale overflow into L); the JIT unconditionally cleared V and did not
latch actual overflow. Blame dates those choices to `fdebd6e9` (2021) and
`402a280c` (2022), respectively, predating the MD/MM integration.

The correction explicitly detects the minimum accumulator and updates V/L,
preserving C. Interpreter negation uses unsigned subtraction to avoid signed
64-bit overflow with the left-aligned minimum accumulator. Thirty shared
synthetic cases cover five input boundaries, both accumulators, and three
initial flag states. They check results, V, sticky L, and unchanged C. The
unmodified ARM64 JIT failed the new V assertion; the corrected ARM64 full
core suite passed in 1.98 seconds (JIT and interpreter). The x86-64 full core
suite passed in 2.60 seconds. The rebuilt ARM64 differential diagnostic reports
NEG and ASR parity, but still exits 1 for ASL and ADDL flag mismatches.
Saturation mode and parallel-move scaling/limiting are not established
by these cases. This is a core arithmetic correction, not evidence that the
remaining MM audio or transport failures are resolved.

## ASL alignment and limit flags (September 5 follow-up)

[DSP56300FM ASL, page 13-15](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
defines carry as the last discarded bit (zero for count zero), and overflow
as any change of bit 55 during shifting. Standard L semantics latch V.
The ARM64 implementation still sampled bit 56 and used right-aligned overflow
logic after `1c0af46b2` (August 19) changed its sign-extension calls to helpers
that preserve left alignment. It now explicitly converts to a raw right-aligned
temporary, performs the shift and flag computation there, and restores the
accumulator layout. Zero-extension before shifting ensures count zero clears C.
Both JIT backends now latch ASL overflow into L.

Forty shared cases use a bit-by-bit oracle: five values, counts 0/1/8/55,
immediate/register counts, and A-to-B source preservation. They verify result,
C, V, and L in normal 56-bit arithmetic mode. The old ARM64 implementation
failed the new L assertion; earlier differential evidence independently showed
incorrect carry. The corrected full ARM64 suite passed in 2.17 seconds and
the full x86-64 suite passed in 2.79 seconds.
Its rebuilt differential diagnostic reports ASL, ASR and NEG parity; ADDL still
fails, so the diagnostic remains a failing investigation tool, not a green gate.

ADDL follow-up: the reported input A=`015a7b3f37c905`, B=`d55ad0723547d7`
does not overflow when A is doubled. The unsigned 56-bit sum is below 2^56,
so the interpreter's set carry is not justified by this case. DSP56300FM
page 13-9 additionally defines V for overflow in either the shift or addition;
the interpreter currently clears V unconditionally. These are next investigation
leads. They must not be hidden by masking all ADDL carry/overflow differences
as undefined. No new firmware audio result or hardware equivalence is claimed.

## ADDL correction and renewed firmware measurement

[DSP56300FM page 13-9](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
defines ADDL overflow from either doubling D or adding S, with carry guaranteed
when doubling does not overflow. Both backends previously cleared V; the
interpreter also inferred C from accumulator sign changes instead of unsigned
addition carry. Interpreter blame identifies `b0b707ab`/`c70d3743` (2021),
before MD/MM integration. The correction uses unsigned arithmetic for the
interpreter, computes shift/addition sign overflow, latches L and calculates
addition carry. JIT arithmetic extends to host bits 63:8 so host carry matches
the accumulator boundary, then computes overflow from the operand/result signs.

Seventy-two shared cases cover six D values, six S values, and both accumulator
destinations, preserving S. A signed 64-bit mathematical oracle independently
checks results and V/L; C is asserted only without pre-shift overflow. The old
ARM64 JIT failed the new V assertion. My initial JIT correction used too many
temporary registers on x86-64, producing InvalidInstruction diagnostics and a
test segfault; it was not published. Using a locked accumulator reference for
D instead of another scratch accumulator fixed this implementation error.
Final full core suites pass on ARM64 (2.30 s) and x86-64 (2.95 s).

The rebuilt 17-instruction, 1024-sample-per-instruction differential diagnostic
now exits zero on both architectures, including NEG/ASR/ASL/ADDL. This is a
bounded synthetic comparison, not comprehensive ISA correctness or an oracle
for saturation/parallel moves. It remains a manually invoked diagnostic.

The interpreter firmware binary was rebuilt with the arithmetic corrections
and `mmSineFirmwareTest` was rerun with the authorized MM image. It still fails
after 49.68 s at the strict idle check: RMS `2.51706e-6`, unchanged from the
previous failing measurement. No check was relaxed. Thus these arithmetic
corrections do not resolve the observed idle-noise failure; transport and other
execution differences remain open. The final JIT-only register-allocation
adjustment does not alter that interpreter path. Fresh JIT firmware audio
validation and the broader goal checklist remain required.

## Idle-output characterization after arithmetic fixes

The ARM64 JIT `mmSineFirmwareTest` was rebuilt at the ADDL correction and
passed in 58.00 seconds, including the existing level, reduced-rate, and
note-sweep assertions. The sine-oracle controls also passed (0.19 seconds).

The firmware test now reports per-channel mean, RMS, AC RMS and absolute peak
over consecutive 4096-frame windows of its existing 16384-frame idle sample.
These are passive observations of the exact samples used by the gate: no extra
settling, DC removal from the gate, threshold changes, or runtime changes.
The instrumented interpreter test still fails at aggregate RMS `2.51706e-6`
(76.10 seconds while another architecture test ran concurrently).

Across its eight channel/windows, means range approximately -4.05e-7 to
-4.85e-7, AC RMS ranges 3.78e-7 to 3.84e-6, and the largest peak is
6.48499e-5. The last window still has peaks 6.12736e-5 (left) and 4.07696e-5
(right). Thus a constant DC offset does not explain the failure; substantial
time-varying output remains throughout this roughly 0.37-second observation.
This does not establish its cause, spectrum, duration beyond that window, or
physical-device behavior. It motivates inspecting sample/peripheral ordering
and remaining execution differences, not bypassing the strict silence check.
The instrumented x86-64 JIT firmware sine test passed in 120.39 seconds;
every idle window/channel reported exact zero for all four statistics. That
provides a passing-backend comparison for the same measurement path, not proof
that hardware must be perfectly silent.

## Repeated arithmetic coverage and execution-mode constraints

A per-DSP hybrid interpreter/JIT experiment is not presently a valid simple
dispatcher toggle. `DSP::execOp` and `rep_exec` accumulate interpreter cycles
only in non-JIT builds; cycle-cache allocation/invalidation and fast-interrupt
execution also depend on compile-time `g_useJIT`. Calling `execInterpreter`
inside a JIT build can compare isolated register operations, as the synthetic
diagnostic does, but does not reproduce the standalone interpreter's timed
machine execution. No hybrid firmware result is claimed.

Both the interpreter REP loop and generated JIT REP body execute without
ordinary dispatcher returns. [DSP56300FM page 2-15](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf?WT_ASSET=Documentation)
states that interrupts are not serviced during the repeated body; this must
not be confused with stopping peripheral clocks. The current observation does
not distinguish the backends or prove that changing REP interruptibility would
fix audio. Peripheral timing and IRQ delivery need separate evidence.

The firmware-free diagnostic now tests each of its 17 arithmetic/move bodies
singly and with immediate REP counts 4 and 16, each with 1024 deterministic
inputs and three scaling modes, also comparing LC. All 51 combinations pass
on both ARM64 and x86-64. This expands coverage of generated repetition paths
but is still register/PC/LC comparison against another emulator backend, not
a cycle/interrupt oracle. It does not cover REP zero/register counts, memory
operands, DMA, serial ports, or saturation mode. No firmware runtime code or
audio threshold changed in this increment; the idle failure remains unresolved.

## Normal-dispatch cycle probe

`mdDspArithmeticParityTest --cycles` now runs in either a JIT or forced-interpreter
build. It uses a fresh synthetic machine per case, a one-instruction JIT block
cap, and the normal `execUntilCycles(start + 1)` path. It checks the exact ending
PC and restored LC before reporting cycle and instruction-count deltas. Unlike
the register-parity mode, it does not call the interpreter inside a JIT build.
It uses NOP and ADD X0,A alone and with immediate REP counts 4 and 16; no ROM,
interrupt source or active serial/DMA peripheral is supplied.

All six cases agree across ARM64 JIT, x86-64 JIT, and forced ARM64 interpreter:

| Body execution | Cycle delta | Instruction-counter delta |
| --- | ---: | ---: |
| Single NOP or ADD | 1 | 1 |
| REP #4, NOP or ADD | 9 | 5 |
| REP #16, NOP or ADD | 21 | 17 |

This excludes a basic backend cycle-count discrepancy for these particular
internal-memory programs. The implementations share timing tables, so agreement
does not independently establish hardware timing. It does not establish when
ESSI/DMA work becomes visible inside a long repeated operation, nor cover
memory wait states, fast/long interrupts, or firmware execution. Those remain
next timing targets; the MM idle-noise and host-loss requirements remain open.
The manual diagnostic's CMake description was updated because the original
arithmetic mismatches are now fixed, while its coverage remains bounded.

## Synthetic host-command interrupt timing

`mdHdi08HardwareTest --timing` uses the existing ROM-free DSP56303/HI08 fixture
with a one-instruction JIT block cap, disabled block linking, and dynamic fast
interrupt mode. It requests a single enabled host command, observes acceptance
through the public core callback, checks a synthetic register marker, and checks
return to the synthetic main loop. All observations occur through normal
`execUntilCycles` dispatch in each separately built backend. The fixture now
reports a short JSR/RTI handler, the same handler with 128 NOPs, and a two-word
MOVE/NOP fast interrupt. No private firmware state or disassembly is used.

ARM64 JIT, x86-64 JIT and forced ARM64 interpreter all report these cycle
deltas from command submission:

| Synthetic handler | Acceptance | Marker observed | Return observed |
| --- | ---: | ---: | ---: |
| JSR, MOVE marker, RTI | 3 | 7 | 10 |
| JSR, 128 NOPs, MOVE marker, RTI | 3 | 135 | 138 |
| Fast MOVE marker, NOP | 3 | 8 | 8 |

Marker/return timestamps are dispatcher-boundary observations, not individual
instruction retire timestamps; fast-interrupt execution can complete within one
dispatch. These matching measurements do not independently prove hardware
latency or exclude nested interrupts, memory wait states, DMA/ESSI deadlines,
or reentrant host transport. No runtime timing change is justified by these
cases. The existing no-argument HI08 regression suite also passes on ARM64 JIT
(0.11 s), interpreter (0.08 s), and x86-64 JIT (0.19 s). The MM audio root cause,
lossless host transport, and remaining MD panel dependency are still unresolved.

## ESSI dispatch-boundary measurements

`mdEssiDispatchTimingTest` is a new manual ROM-free diagnostic using the real
DSP56303 ESSI/clock implementation and normal `execUntilCycles` dispatch. It
compares a loop of 128 NOPs with REP #128,NOP, using JIT block caps 1 and 32.
Synthetic CRA settings select one-slot frames at 16 cycles (8-bit, PM=0,
PSR=1) or 96 cycles (24-bit, PM=1, PSR=1). Only TX0 is enabled; interrupts,
RX, DMA, peer DSPs and firmware are absent. A bounded callback records actual
peripheral-service cycle timestamps, not hardware wire-edge timestamps.

ARM64 and x86-64 JIT results agree exactly. The forced ARM64 interpreter agrees
with the one-instruction JIT configuration; changing its unused JIT block cap
does not affect its measurements. Maximum observed lateness relative to the
configured slot schedule is:

| Slot period | Body | Interpreter / JIT cap 1 | JIT cap 32 |
| --- | --- | ---: | ---: |
| 16 cycles | 128 ordinary NOPs | 0 | 25 |
| 16 cycles | REP #128,NOP | 125 | 128 |
| 96 cycles | 128 ordinary NOPs | 0 | 9 |
| 96 cycles | REP #128,NOP | 117 | 120 |

For 96-cycle ordinary slots, the first five callback timestamps are
96,192,288,384,480 with interpreter/cap-1 JIT, versus
96,195,294,390,489 with cap-32 JIT. REP produces timestamps
133,269,405,405 with interpreter/cap-1, versus 136,272,408,408 with cap-32.
Thus an exact cycle deadline does not imply exact service inside a JIT block
or REP. Overdue fine-clock slots are delivered together on dispatcher return.

The loop stops after the first dispatch reaching at least 512 cycles; actual
stop cycles are printed and may overshoot. It deliberately does not flush
pending peripheral work at exit. Different callback totals are therefore not
evidence of permanent slot loss. The immediate TX action when CRB enables the
transmitter occurs before measurement; the table covers subsequent clock ticks.
The test checks that measurement is populated, bounded, and never precedes the
configured deadline; it does not assert that measured lateness is acceptable.
All eight cases execute successfully in all three builds. Existing compiler
warnings about a negative signed shift in dsp.h remain unrelated/unfixed.

This establishes a concrete backend/configuration-dependent service boundary,
not the MM idle-noise root cause or independent physical timing correctness.
The next discriminating firmware experiment is JIT block-cap sensitivity with
unchanged audio gates; subsequent transport/DMA testing must distinguish
peripheral progress from interrupt acceptance during REP. Do not make the
interpreter artificially late, alter REP interruptibility, or tune settling/noise
thresholds merely to match the currently passing JIT. No runtime code or audio
acceptance threshold changed in this increment.

## MM sine sensitivity to JIT dispatch granularity

The audio fixture now has three manual diagnostic modes:
`--sine-jit-single-instruction` (both DSPs), `--sine-jit-single-mixer` and
`--sine-jit-single-producer`. Each changes only the selected DSP's
`maxInstructionsPerBlock` from the normal 32 to 1, before scheduled boot starts.
The normal sine setup, fingerprint check, settling, finite/idle/level checks,
roughness bounds and subsequent parameter/note sweeps remain intact. Interpreter
builds explicitly reject these JIT-only modes; this rejection was tested.
REP execution and the existing `maxDoIterations=4` configuration are unchanged,
so this is not a claim of complete instruction-by-instruction peripheral service.

With both DSPs capped at 1, ARM64 and x86-64 independently report identical
results: idle RMS 0, track-0 RMS 0.112433, zero-level RMS 0, roughness
0.00253165, then exit 1 at the strict smoothness/pitch-scale check. A fresh
default ARM64 `--sine` run passes all six tracks and note/parameter sweeps;
track-0 RMS is 0.113775 and roughness 1.92966e-6. Thus the baseline passing
result is configuration-sensitive; smaller blocks are not a validated fix.

Reducing only the mixer to cap 1 preserves silent idle but fails on both
architectures, at different checks. ARM64 fails track 0 immediately: RMS
0.11348, zero-level RMS 0, roughness 0.00108895. x86-64 passes that initial
check (RMS 0.113501, roughness 1.92945e-6), then fails the restored-sine check
after the sample-rate-reduction/parameter burst. Its reduced-rate RMS is
0.0268692 and roughness 0.000522186; the restored measurement is not currently
printed. Do not describe the mixer-only runs as numerically identical.

Reducing only the producer to cap 1 on ARM64 completes all six tracks and
sweeps with exit 0. Track-0 RMS is 0.1132, roughness 1.92945e-6. However,
zero-level observations on tracks 1–5 are nonzero (maximum 0.000525805),
unlike the fresh default run, and track-5 note 72 has RMS 0.00372497.
These pass the existing relative attenuation/non-silence checks but are not
evidence of waveform or level equivalence. x86-64 producer-only was not run.
The sine-oracle unit test still passes. Diagnostic modes are not registered
as green CTests: their failures are evidence to investigate, not acceptance
criteria to relax.

These measurements concern observable MIDI/audio behavior, without private
firmware inspection. They establish JIT configuration sensitivity, not its
unique cause: changing block boundaries also changes generated-code grouping,
register/flag materialization and scheduling interactions. ESSI batching is a
candidate, not proven causal. They also do not reproduce the interpreter's
nonzero idle output, so the interpreter requirement remains unresolved. No
production runtime setting changed. Next work must isolate mixer peripheral,
DMA/link/host transport timing from code-generation boundary effects rather
than choosing the block cap with the most convenient audio result.

## Reproduced DMA transfer-done status gap

Auditing mixer DMA state found a concrete shared-core omission independent of
firmware. `Dma` initializes all six DSTR.DTD bits to one, but channel enable
and completion never update those bits. DACT/DCH are updated separately;
they do not substitute for per-channel completion status.

[DSP56300FM Rev. 5, table 10-10 and section 10.6](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
describe clearing a channel's DTD bit after the DE-enable pipeline delay
(three instruction cycles), and completion/disable behavior. They also require
preserving an already captured word request when disabling a channel. Therefore
an immediate blanket cancellation or simply deriving DTD as inverse DE would
not constitute a faithful replacement. The public manual was checked directly
using the local PDF in Downloads, not firmware disassembly.

New manual executable `mdDmaStatusRepro` creates a fresh ROM-free DSP56303 for
each of six channels. It enables word/request/clear-DE mode with external IRQA
as source, but supplies no request, interrupt or active serial peripheral. After
16 normal-dispatch NOP cycles, it checks that DE remains set and DSR/DDR/DCO
are unchanged. Only that channel's DTD should have cleared by this checkpoint.
ARM64 JIT, x86-64 JIT and forced ARM64 interpreter all reproduce the same error:
the done mask remains decimal 63 for every channel, rather than 62,61,59,55,47,31
respectively. Each executable exits 1 as intended. It is not registered as a
passing CTest; this is a public-spec regression to turn green with a real fix.

The local history contains the same initialization/accessor and omission in
`402a280ca` (2022-12-04), predating MD/MM integration. This establishes inherited
history in this checkout, not the state of present upstream or authorship intent.
No DMA runtime code changed in this increment. DTD polling's involvement in MM
audio has not been established; this finding must not be called its root cause.

The next implementation needs explicit enable-pipeline, completion and disable
transitions, including re-enable before a deferred update, independent channel
state, request-driven versus block transfers and public status reads. Extend
the synthetic coverage to those boundaries before firmware validation. Preserve
already-captured request semantics; do not mask this gap through firmware-state
checks or assume that a passing idle test proves DMA correctness.

## DMA status correction and independent core regression

The DTD omission now has a shared-core correction: enabling a channel schedules
its status clear after three instruction counts in the peripheral dispatcher,
even if no delayed DMA block owns DACT. Completion/disable cancels stale status
updates; re-enable replaces the deadline. A request after a completed
continuous-mode block marks it busy again. Synchronous request transfers have
no deferred captured request after their callback returns, while an already
accepted delayed block still completes before reporting done after disable.
No firmware predicate, payload check or private-memory dependency is added.

The original `mdDmaStatusRepro` has been expanded and moved into the DSP repository
as `source/dsp56kTestRunner/dmaStatusTest.cpp`, executable `dspDmaStatusTest`,
CTest `dsp56300_dmaStatus`. Its former main-repository target/file were removed;
the original red reproducer remains in history. This keeps the fix, passing
regression, and `doc/dma_status_validation.md` together for independent review
and a potential upstream submission. No separate upstream PR or merge occurred.

All six channels are checked for enable-delay boundaries, ordinary dispatcher
wake, word/block completion, actual copied data, disable/re-enable generations,
early completion, continuous-mode re-requests and accepted delayed work after
disable. The expanded regression and full core suite pass on ARM64 JIT,
x86-64 JIT and forced ARM64 interpreter. ARM64 HI08 hardware regression also
passes. Fresh firmware results with production block caps and thresholds intact:

- ARM64 MM strict sine: pass all six tracks and note/parameter sweeps.
- x86-64 MM strict sine: pass all six tracks and note/parameter sweeps.
- ARM64 MD UW/RAM: pass, ROM peak 0.276559; RAM correlations 0.990227–0.991797.
- ARM64 interpreter MM sine: unchanged idle failure, RMS 2.51706e-6 (exit 1).

Confidence is high in the reproduced missing status transition and its bounded
regressions, not in complete DMA timing. Bus arbitration, asynchronous request
capture, controller-wide reset and timing inside long instructions/REP remain
outside this correction. The enable-delay oracle uses NOPs and cap-1 dispatch.
The earlier mixer block-cap audio failures were not rerun in this increment.
MM host-word loss and panel remediation remain open; this fix does not complete
the larger goal or establish the interpreter audio root cause.

## Explicit assignment and post-setup cache controls

`mmAudioFirmwareTest --sine-midi-jit-single-instruction` combines the existing
documented GND-SIN SysEx assignment fixture with cap-1 execution on both DSPs
from startup. ARM64 and x86-64 agree: idle RMS 0, track-0 RMS 0.112555,
zero-level RMS 0, roughness 0.00107273, then exit 1 at the strict sine-quality
check. Reassigning GND-SIN therefore does not remove this configuration's
failure. This fixture still includes the empty-kit panel prelude and extra
assignment/processing time; it does not independently prove parameter receipt
or isolate setup from transport. The normal production block caps are unchanged.

Two further manual modes use default cap-32 boot/setup/idle, then rebuild the
JIT caches outside CPU execution, without changing firmware memory:

- `--sine-jit-single-playback` switches both DSPs to cap 1 at that boundary.
- `--sine-jit-recompile-playback` rebuilds at the unchanged cap 32 as a control.

Both ARM64 runs pass pre-switch idle but then fail the relative level-control
check. The cap-1 run reports track-0 RMS 0.173828, zero-level RMS 0.132183,
roughness 4.0606. The unchanged-cap control reports 0.174532, 0.132183 and
4.05787 respectively. **The control fails, so these runs do not isolate a
block-cap effect.** x86-64 post-setup controls were not run. Interpreter builds
reject these JIT-only modes; the rejection was checked. Restored-SRR RMS and
roughness are now printed before their existing assertion for clearer failures.

A ROM-free control, `mdDspArithmeticParityTest --cache-loop`, reproduces an
active-DO cache-invalidation defect on both ARM64 and x86-64. It assembles a
five-iteration loop adding B to A, pauses after the first addition, and either
leaves the cache intact or calls `destroyAllBlocks()` without changing config.
With the cache intact it reaches synthetic PC 0x104 with A=0x5000000 and restored
LC=0x321. After clearing, it reaches the same PC with A=0x1000000 and LC=5;
the remaining iterations and loop teardown did not happen. Each executable
returns 1 because the cache-clear branch fails; the paired control passes.
The original 51-case arithmetic diagnostic still passes on both architectures.

Source inspection shows cache block destruction removes the JIT's loop metadata.
This is an independently reproduced issue relevant to the new experimental
cutover, not an established explanation for the original MM failures, which do
not clear the cache. No private firmware PC, loop or memory was inspected.
These diagnostic failures are retained for visibility, not registered as passing
acceptance gates. Before using the playback cutover to reason about MM, establish
cache recompilation that preserves active execution semantics when program memory
is unchanged, separately from invalidation after program replacement. Do not
blindly preserve obsolete loop metadata when the underlying program has changed.

## Loop-preserving recompilation and a usable playback control

Added `Jit::recompileAllBlocks()` for recompiling unchanged program memory while
retaining known DO/DOR descriptions. It is distinct from `destroyAllBlocks()`
for program replacement. Full invalidation now explicitly clears retained loop
maps even when no compiled setup block owns them; program-write notification
also invalidates descriptions when either DO/DOR setup word changes. The API
must be called outside DSP execution, allocates memory, and is not a real-time
or thread-safety guarantee. No production synth caller was migrated.

The single/nested synthetic DO controls now finish correctly on ARM64 and x86-64
with or without recompilation. Equivalent tests live in the DSP repository's
`JitUnittests::recompileActiveLoops`, including retained-metadata invalidation
after changing the outer loop endpoint and full cache destruction. The full
core suite passes on ARM64 (3.62 s), x86-64 (4.38 s), and forced ARM64 interpreter
(3.57 s); the new JIT-only execution test is skipped in the latter. The core
API, regression and `doc/jit_recompilation_validation.md` form an independently
reviewable change. The main `--cache-loop` diagnostic now uses recompilation
and covers both single and nested loops rather than treating destructive
invalidation as an unchanged-program operation.

After this correction and the metadata-invalidation checks, the post-setup
audio comparison is usable. Both machines boot, clear the kit and pass idle
with normal block caps. Only then is compiled code rebuilt:

| Post-setup operation | ARM64 JIT | x86-64 JIT |
| --- | --- | --- |
| Recompile, retain cap 32 | Pass all six tracks/sweeps | Pass all six tracks/sweeps |
| Recompile, change both DSPs to cap 1 | Fail first-note sine quality | Fail first-note sine quality |

Both cap-1 runs report idle and zero-level RMS 0, first-note RMS 0.112626 and
roughness 4.9428e-5. Both cap-32 controls report first-note RMS 0.113062 and
roughness 1.93014e-6. Later measurements differ across architectures; passing
checks are not a claim of bit-identical output or exact timing equivalence.
The ARM64 MD UW/RAM regression also passes after the final core changes, with
ROM peak 0.276559 and RAM correlation range 0.990227–0.991797.

This shows that cap-1 execution during startup/kit setup is not required to
produce a sine failure: a post-setup change suffices. It still does not separate
generated-code grouping/register effects from peripheral/host/link scheduling,
and it does not establish correct receipt of subsequent parameter traffic.
The interpreter idle failure was not rerun in this increment and remains open.
No audio thresholds, firmware checks, production block caps or private-state
remediations were weakened. Next isolate code-generation boundaries with
ROM-free instruction sequences and memory/flag checks before attributing this
post-setup failure uniquely to transport timing.

## ROM-free instruction-pair discrepancy after CLR

`mdDspArithmeticParityTest --sequences` compares 361 ordered pairs of 19
one-word arithmetic, accumulator-move and conditional-transfer instructions.
Each pair uses up to 1024 deterministic inputs across three scaling modes,
stopping that pair at its first mismatch. It compares A/B/X/Y/SR and checks
the endpoint after two interpreter instructions, two single-instruction JIT
dispatches, and one grouped JIT dispatch ending in an explicit self-jump.
Peripherals are no-op fixtures; neither backend is an independent ISA oracle.
This is an opt-in diagnostic, not a passing acceptance gate.

Both ARM64 and x86-64 report `Sequence cases 361 failures 15` (exit 1).
All failing pairs end in `clr a`; the single and grouped JIT results agree,
but differ from the interpreter's SR. For example, `abs a; clr a` with
A=0x015a7b3f37c905, B=0xd55ad0723547d7, X=0xa4cb864a3898,
Y=0x801dd1098ae9 and SR=0xbc ends with A=0 in all backends, but interpreter
SR=0xa4 versus JIT SR=0x94. The other failing predecessors are NEG, ADD
(X0/B), SUB (X0/B), MAC, MPY, MPYR, MACR, RND, ASR, ASL, ADDR and ADDL.

The initial harness used unsupported assembler spellings for its jump and
conditional transfers; those assembly errors were corrected before collecting
these results. Final conditional transfers are `tcs x0,b` and `tlt x0,b`.
The diagnostic prints inputs and all three resulting register sets on failure.

This provides a focused interpreter flag-lifetime lead; it does not establish
the cause of MM idle noise, hardware-correct flags, or a grouped-JIT defect.
Next check CLR's public ISA flag requirements and the interpreter's pending
flag handling, add an independent core regression, and rerun the strict MM
interpreter gate if a correction is justified. No core behavior, firmware
workaround or audio threshold changed in this increment.

## CLR stale-flag correction and unchanged interpreter audio failure

Public DSP56300FM Rev. 5 page 13-44 requires fixed E/N/V=0 and U/Z=1 for CLR.
The interpreter wrote these flags without retiring its preceding arithmetic
result. A later status read recomputed E/U/N from that older result. Retiring
pending flags before CLR installs its result fixes the reproduced discrepancy.
The core change, specification-derived regression and historical evidence are
independently documented in `source/dsp56300/doc/clr_flag_validation.md`.

The initial ASR/CLR core regression fails before the change. Afterward the
expanded 16-case matrix and entire core suites pass on ARM64 JIT (2.54 s),
x86-64 JIT (3.00 s) and forced ARM64 interpreter (2.42 s). The 361 ordered-pair
diagnostic reports zero failures on both JIT architectures; all existing 51
instruction/repeat comparisons pass too. Normal MM sine tests pass both JIT
backends, and ARM64 MD UW/RAM passes with unchanged ROM peak 0.276559 and RAM
correlations 0.990227–0.991797.

The MM interpreter test still fails at idle with RMS 2.51706e-6, unchanged
from the preceding revision and above the strict 1e-7 gate. CLR is therefore
a validated core correction, not a demonstrated fix for the MM idle symptom.
No thresholds, firmware checks or removed private-state hooks were changed.
The remaining transport, interpreter, panel-evidence and broader validation
requirements in the goal checklist are still incomplete.

## Logical instruction sequences and CLB correction

The optional `--sequences-logical` diagnostic expands the existing matrix to
35 instructions / 1225 ordered pairs, adding logical operations, rotates,
CLB, compare/test, NOP and additional condition consumers. It still compares
interpreter, single-instruction JIT and grouped JIT with no peripherals and
up to 1024 deterministic inputs per pair. It stops each pair at its first
mismatch; a failure count is neither a count of independent bugs nor exhaustive
evidence about every input. An indirect self-jump through R0 avoids the local
assembler's 12-bit absolute JMP limit; the original 361-pair control still
passes on both architectures with that endpoint. Both PCs and R0 are checked.

Pairs can be isolated without changing their matrix program address:

```sh
mdDspArithmeticParityTest --sequences-logical 'nop' 'clb a,b'
mdDspArithmeticParityTest --sequences-logical 'clb a,b' 'move a,x0'
mdDspArithmeticParityTest --sequences-logical 'abs a' 'tnr x0,b'
```

The expanded matrix exposed CLB flag defects independently confirmed against
DSP56300FM Rev. 5 page 13-42. Both JIT implementations computed N from the
destination before writing the result, at the wrong bit position; the host
register could be unloaded. ARM64 also computed Z without testing the count.
The interpreter could overwrite CLB's N from a preceding deferred result.
The correction derives flags from the newly installed count, explicitly tests
it for Z, and retires the interpreter's preceding flags. Negative count
placement also uses an unsigned shift to avoid undefined C++ behavior.

The core regression checks all possible counts with positive/complemented
operands, source zero, source all ones, same/separate destinations and an
ASR/CLB pair. It fails both JIT suites before the correction. See the standalone
core note `source/dsp56300/doc/clb_flag_validation.md` for provenance, coverage
and validation. The two focused CLB pairs above now pass both architectures.

The larger matrix remains a deliberately failing diagnostic: after CLB's flag
correction it reports 223 failing pairs on ARM64 and 247 on x86-64. Earlier
x86-64 totals varied across runs, consistent with but not uniquely proving an
uninitialized-register dependency. Other instruction families still need
independent checks; do not interpret the post-fix totals as exhaustive.

One remaining ARM64 witness is `abs a; tnr x0,b`, trial 532: input
A=0xff55e39dba8f13, B=0x896b98d85a5ecd, X=0xed3b46c4f930,
Y=0x8a4a67d53cc2, SR=0x43b. All three finish at the expected PC with SR=0x403,
but grouped JIT writes B=0 while interpreter and single JIT transfer X0 into B.
There are also x86-64 grouped/single discrepancies around LSL/LSR. These are
register-only synthetic leads; they have not established the cause of MM's
block-size sensitivity. Public table 12-17's condition equations must be
checked independently too: backend agreement alone cannot validate them.

After the final unsigned count-placement cleanup, core suites pass ARM64 JIT
(2.65 s), x86-64 JIT (4.47 s), and forced ARM64 interpreter (3.51 s). The
original 361-pair matrix and both focused CLB controls pass both architectures.
Normal MM sine remains passing on both JIT backends; ARM64 MD UW/RAM passes
with ROM peak 0.276559 and RAM correlations 0.990227–0.991797. Interpreter MM
idle still fails with RMS 2.51706e-6. The ARM64 post-setup cap-1 diagnostic also
retains its preceding first-note failure (RMS 0.112626, roughness 4.9428e-5).
No MM audio thresholds, production block caps or private-state remediations
were weakened. The larger logical matrix and all unchecked goal requirements
remain open.

## ARM64 NR/NN temporary-register pressure

The remaining `abs a; tnr x0,b` grouped-JIT discrepancy is caused by temporary
register pressure during deferred flag resolution. A transfer operand, two
condition temporaries, and two U-computation temporaries overlap in a pool of
four registers. The allocator's assertion-only boundary allowed empty-vector
access when assertions were disabled. Resolve E/U/Z before reserving both
condition temporaries; retain the transfer operand throughout. Also reject
exhausted strong/weak acquisitions with a checked error rather than generating
code from an invalid or already-live register. This is not a spill/fallback or
an assertion that every JIT error is recoverable.

An independent 24-case core regression checks actual cap-1/cap-32 dispatch,
three scaling modes, NR/NN taken/untaken cases, PC and registers. It fails
before the change and passes afterward. A pool regression checks exhaustion,
release/reuse and weak-register reclamation. The standalone core note
`source/dsp56300/doc/conditional_transfer_register_pressure.md` records exact
evidence, historical boundaries, and limits (including un-attributed pre-fix
signal exits, which are not treated as successful reproductions).

Final core suites pass ARM64 JIT (2.61 s), x86-64 JIT (4.09 s), and forced
ARM64 interpreter (3.04 s). The focused ARM64 pair passes all 1024 inputs.
The larger logical matrix now reports 208 failing pairs on ARM64, down from
223; x86-64 remains at 247. These are first-witness counts, not independent
bug counts or exhaustive backend-equivalence results.

With the final guard, normal MM sine tests pass both JIT architectures and
ARM64 MD UW/RAM passes (ROM peak 0.276559, correlations 0.990227–0.991797).
After the decoding correction, the ARM64 post-setup cap-1 audio diagnostic
still fails with unchanged RMS 0.112626 and roughness 4.9428e-5. The previous
interpreter idle failure was not rerun in this JIT-only increment. No private
firmware hook or threshold was changed, and the overall goal is incomplete.

Next: audit partial flag writes in the interpreter, where logical N updates
can coexist with pending arithmetic E/U/N, and verify all compound condition
truth tables against public table 12-17. The current code's zero-normalization
and compound signed equations cannot be validated by backend parity: multiple
backends share the same expressions. Keep those corrections separate from
this register-lifetime fix and rerun the strict MM gates after each change.

## Central interpreter partial-flag correction

The interpreter's logical operations directly replace N/Z/V while preserving
the arithmetic instruction's E/U/C. The deferred-flag cache previously kept
the old N pending and recomputed all E/U/N whenever any pending bit remained.
A later status read could therefore restore the preceding arithmetic N over
the logical result. A ROM-free `neg a; and x0,a` pair reproduces this; public
DSP56300FM logical instruction definitions establish the required behavior.

The core now retires explicitly written CCR bits, resolves only the pending
E/U/N subset, and resolves preserved pending bits before replacing the saved
arithmetic result. This addresses the shared cache contract rather than
adding a separate flush to every logical instruction. Full-SR replacement,
mode-bit helpers, disabled S handling, JIT generators, instruction timing,
and the already validated CLR/CLB flushes are unchanged. See
`source/dsp56300/doc/partial_ccr_write_validation.md` for independent evidence,
historical provenance and limitations.

The new 96-case ASR/logical sequence regression fails the forced interpreter
before the correction; it checks both accumulators and preserved/replaced
flags without an intermediate status read. Another 168 cache-contract cases
cover partial source replacement and explicit mask/bit writes. Core suites
pass ARM64 JIT (3.41 s), x86-64 JIT (4.22 s), and forced ARM64 interpreter
(2.97 s). The original 361-pair matrix and 51 arithmetic/repeat cases pass
both architectures. The expanded logical diagnostic drops from 208 to 107
failing pairs on ARM64 and from 247 to 179 on x86-64. Remaining ARM64 first
witnesses all involve rotates; x86-64 additionally has logical-shift failures.
These totals are diagnostic witnesses, not independent bug counts or hardware
equivalence evidence; the shared compound-condition audit is still needed.

Normal MM strict sine tests pass both JIT architectures, including all six
tracks and parameter sweeps. ARM64 MD UW/RAM passes (ROM peak 0.276559,
RAM correlation 0.990227–0.991797). The forced-interpreter idle gate is
unchanged and still fails at RMS 2.51706e-6.
The post-setup cap-1 diagnostic was not rerun in this interpreter-only
production correction; its last recorded failure remains unresolved. No
threshold, private-hook removal, transport behavior or block cap changed.
The full goal checklist remains incomplete.

During this rebuild, x86-64 compilation hit a full filesystem. Two completed
test logs, `/private/tmp/mm-do-firmware.log` and
`/private/tmp/mm-do-count-firmware.log`, were compressed to `.log.gz`, retaining
their contents and recovering about 180 MB. No worktree/source cleanup was
performed; the failed build was retried successfully. Disk capacity remains
tight and should be checked before further large builds or traces.

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
