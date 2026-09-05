# MD/MM cleanup: results, evidence and limits

Written 2026-09-05 at the user's request, against main `b7007a00` (runtime
integration `8318b821`), DSP `8e4708b0`, and MCU `62dc26cd`.

**Status: saved for a future release, unfinished and unmerged.** This note
summarizes work completed to date; it does not authorize resuming the deferred
cleanup. The chronological [remediation journal](md_mm_firmware_hook_remediation.md)
contains per-experiment revisions, results and reversions. The
[acceptance checklist](md_mm_remediation_goal_checklist.md) remains incomplete.

## Relationship to the original reported bugs

The original Classic/Extended label reversal, garbled RAM playback noise and
RATE-dependent pitched corruption were addressed before this cleanup. Correcting
sample packing also exposed and fixed missing external codec input to DSP2.
Those changes are already merged in
[main PR #42](https://github.com/joelanders/gearmulator-md-mm/pull/42) and
[DSP PR #5](https://github.com/joelanders/dsp56300-md-mm/pull/5); see
[the original validation note](md_ram_audio_validation.md).

This subsequent work investigated firmware-internal dependencies. It is not
another prerequisite for delivering the original fixes. The saved cleanup is in
[main draft #43](https://github.com/joelanders/gearmulator-md-mm/pull/43),
[DSP draft #6](https://github.com/joelanders/dsp56300-md-mm/pull/6), and
[MCU draft #3](https://github.com/joelanders/mc68k-md-mm/pull/3).

## What “manual-backed” does and does not mean

There were three different kinds of evidence, which must not be conflated:

1. **Independent processor/peripheral requirements.** The public
   [DSP56303 User's Manual](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf)
   describes host-interface registers, interrupt enables and acknowledgment.
   The [DSP56300 Family Manual](https://www.nxp.com/docs/en/reference-manual/DSP56300FM.pdf)
   provides instruction and flag behavior. Synthetic tests can check these
   requirements without any Elektron firmware image.
2. **Documented external controls.** The published
   [Monomachine manual](https://www.bhphotovideo.com/lit_files/85386.pdf)
   provides panel operations, MIDI controls and machine assignment messages.
   Tests use these to exercise real firmware and observe its output, instead of
   setting up a sound by writing its private memory.
3. **Empirical evidence that a hook is unnecessary in tested cases.** If normal
   execution works after a hook is removed, that supports removal for those
   cases. It does not establish why the hook originally existed, prove the
   entire emulation is accurate, or make its removal a manual-derived fix.

## Hook-by-hook results

| Existing dependency | Retained cleanup result | Evidence and qualification |
| --- | --- | --- |
| Fabricated DSP boot replies and vector-specific deferral | Removed interception; ordinary host-command and data paths let the DSP execute and generate replies. | MD/MM firmware regressions passed. MM must return kit status through MIDI and keep updating its panel. Exact hardware timing remains unverified. |
| MM sample correction selected by a firmware program location | Removed the DSP-core correction and its integration hook. | Targeted JIT sine tests passed with and without it, across architectures. Its historical necessity is unexplained; interpreter audio fails with it enabled or disabled. |
| MM parameter-transfer guard inspecting private firmware memory | Removed guard and associated word/voice/vector tracking after transport and interrupt corrections. | Removal alone silenced the fifth track under the strengthened test. Corrected transport passed sine/burst and Ensemble tests on both JIT architectures. Remaining compatibility assumptions prevent calling the whole interface correct. |
| Factory DigiPRO waveform copying using private layouts | Removed constructor injection and its layout-specific helper/tests; kept image fingerprint checks. | Firmware-driven DPRO-DDRW and DPRO-DENS sweeps exercised six tracks and the full selector CC range. This does not establish exact waveform identities, former spill-word equivalence or edited-bank/state behavior. |
| Panel-ready task-list manipulation | Removed its use for MM, but retained it for MD. | Removing the MD caller broke first-run UW flash initialization. MM repeated menu/LCD tests passed without it. The MD replacement remains unresolved. |
| Panel startup handshake | Not established as a completed replacement or as inherently inappropriate protocol emulation. | External panel signaling still needs independently justified protocol evidence. Local attribution comments were not sufficient evidence of provenance. |

## Strongest causal example: MM parameter transfer

The parameter test performs 16 rounds of sample-rate-reduction changes across
all six voices while a track sounds, ends at the nominal setting, drains MIDI,
and requires the resulting audio to be a smooth sine again. Boot or readable
kit metadata alone would not establish that the DSP applied the updates.

The staged evidence was:

1. The guarded baseline passed. Removing only the private-memory guard made
   track 5 effectively silent on ARM64 and x86-64, while boot still passed.
2. DSP56303UM table 6-17 distinguishes the host transmit latch from the DSP
   receive latch. The bridge was waiting for its entire receive queue to drain
   before accepting another word. MM two-stage pacing allowed the guard-free
   sine/burst test to pass, but wider Ensemble validation exposed more work.
3. A firmware-free interrupt program then demonstrated delivery with HCIE
   disabled, contrary to table 6-8. Follow-up tests covered disabled/pending
   commands, enable transitions, cancellation, reset, source ownership and
   acknowledgment at acceptance rather than handler return (table 6-16).
4. Corrected interrupt handling and DSP scheduling/wake behavior allowed both
   JIT architectures to pass the sine/burst and full Ensemble cases without
   restoring the private guard.

This is a reproducible failure followed by a hardware-derived replacement,
not just deletion followed by a boot smoke test. It supports the tested
combination of corrections, not a claim that each was individually the sole
root cause. MM queue clamping, buffering and read-side progress assumptions
remain; applying the broader pacing change to MD failed its UW regression,
so MD's legacy pacing was retained. That is a compatibility boundary, not a
claim that the two machines have different physical register capacity.

## How behavior was checked

| Layer | What the tests check |
| --- | --- |
| Firmware-free mechanisms | Tiny synthetic programs execute interrupt handlers and exercise pending/enabled/cancelled/reset states. Receive tests require complete words in order in both byte orders. Core tests check instruction results, preserved fields and flags against independent expectations. |
| Real-firmware boot and panel | Supplied MD UW OS 1.63 and MM OS 1.32b fixtures are fingerprint-checked. MM must finish startup, answer kit status through the MIDI UART, and change LCD contents on repeated tempo-menu entry/exit. |
| MM audio and parameter traffic | All six tracks, five sine pitches, silence/finite-output checks, level attenuation, sample-rate reduction and rapid parameter bursts. The sine check measures normalized second-difference energy, detecting gross pitch-scale/smoothness errors; it is not a complete spectral or bit-exact oracle. |
| DigiPRO coverage | Documented machine assignment plus all 128 selector CC values on each of six tracks: 768 observations per sweep. Require audible, finite output and changing timbre. Retrigger notes to avoid mistaking the default envelope's decay for missing waveforms. |
| Original MD regressions | Preserve mode semantics, quiet RAM recordings, external-input selection and sustained waveform correlation during RAM playback. Also exercise audio queues across sample rates/resamplers and varied block/scheduling patterns. |
| Cross-backend checks | Native ARM64 JIT, x86-64 JIT under Rosetta, and forced ARM64 interpreter where recorded. Synthetic core tests pass across all three; firmware audio coverage is not uniformly passing or exhaustive. |

The main executable sources are
[the host-interface tests](../source/elektron/md/mdLibTest/hdi08HardwareTest.cpp),
[receive-word reproducer](../source/elektron/md/mdLibTest/hdi08ReentrancyRepro.cpp),
[MM boot/panel test](../source/elektron/md/mdLibTest/mmBootFirmwareTest.cpp),
[MM audio tests](../source/elektron/md/mdLibTest/mmAudioFirmwareTest.cpp), and
[MD UW/RAM tests](../source/elektron/md/mdLibTest/mdUwFirmwareTest.cpp).
Missing optional firmware produces a skip, not evidence of successful firmware
validation. Consult the journal for the exact revision tested by each run;
earlier comprehensive runs were not automatically repeated after every change.

## Checking the tests, and rejecting broken proposals

- The sine oracle must accept a generated sine and reject silence, DC, a sine
  with every sixteenth sample zeroed, NaN and infinity. “It produces samples”
  is deliberately not the acceptance condition.
- Several regressions were checked by temporarily restoring the old behavior
  and requiring failure. For example, the command-admission test fails if a
  second command must wait for the first handler to return.
- Backend agreement alone is insufficient: two implementations can share the
  same wrong equation. The compound-condition regression therefore checks all
  256 flag patterns against 16 independently specified conditions, 4,096 cases
  per backend. Its [core evidence note](../source/dsp56300/doc/condition_code_truth_table_validation.md)
  records staged failures and limitations.
- Tests also caught defects in proposed changes, including an intermediate
  x86-64 flag implementation that read uninitialized upper temporary bits.
  That failing version was corrected before publication.
- MD panel-hook removal and broader pacing experiments broke MD initialization
  or execution and were reverted. The last receive-latch reentrancy guard fixed
  its synthetic word-loss test but broke MM startup; it too was reverted, and
  a rebuilt restored baseline passed MM boot again.

These negative results are part of the evidence, not successful fixes or
reasons to loosen the acceptance checks.

## Confidence and unfinished work

Confidence is strongest in narrowly specified mechanisms with independent
expectations, demonstrated pre-fix failure and passing post-fix tests. There is
meaningful regression evidence for the exercised JIT hook removals. There is
not enough evidence to claim that no user-visible behavior anywhere changed,
that all removed hooks were understood, or that the integration is merge-ready.

At the last saved core integration, all three core suites passed. Normal MM
six-track sine/parameter and MD UW/RAM regressions passed on both JIT
architectures. Interpreter MM idle still failed at RMS `2.51706e-6`; the
post-setup one-instruction JIT-block diagnostic still failed sine quality on
both architectures. Those symptoms were not resolved by the later arithmetic
corrections.

Most importantly, measured MM receive-word loss persisted even while the
normal audio test passed. One measured run counted 322,736 nested host-latch
overwrites by its sixth-note snapshot. This demonstrates why green audio
does not prove correct transport. The word-loss mechanism is fixed for MD's
callback-free publication path, not for MM's retained path.

Other unfinished acceptance includes the MD private panel dependency, exact
factory/edited waveform-bank behavior, broader reset/state-restoration and
sustained-panel coverage, other firmware revisions, and physical-device
audio/protocol comparisons. Register-level reset tests are not full device
state-restoration tests. Matching old emulator output is regression evidence,
not an independent hardware reference.

## Scope and future review

The numerous additional DSP flag, arithmetic, loop and DMA findings are ordinary
emulator defects, not evidence of a separate firmware-specific hack for every
core fix. Many have good independent tests but did not improve the remaining
MM symptoms. General instruction audits expanded beyond the causal evidence
needed for hook remediation and should not continue as an open-ended part of
this task.

On an explicitly authorized resumption, keep those independent findings
separately reviewable using the [smaller-review map](md_mm_remediation_review_map.md).
New extracted branches need their own baseline and regression validation;
the map is not evidence that such branches already exist. Focus further core
changes on demonstrated connections to unresolved MD/MM failures, and do not
mark the full cleanup complete based only on its passing subsets.

Public documentation and firmware-free synthetic tests strengthen the technical
basis of replacements. They do not by themselves establish clean-room provenance
or legal clearance. No such claim is made here.
