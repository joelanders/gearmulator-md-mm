# Machinedrum mode and RAM audio validation

This note records the investigation, fixes, validation evidence, and remaining
risk for:

- reversed Classic/Extended mode behavior as presented by the UI;
- a garbled noise floor from RAM playback machines; and
- pitched tones introduced by the RAM-R `RATE` parameter.

The changes are split across two repositories and should be merged in dependency
order:

1. [`dsp56300-md-mm` PR #5](https://github.com/joelanders/dsp56300-md-mm/pull/5)
   implements the missing DSP instruction and restores 24-bit DMA address
   wrapping.
2. [`gearmulator-md-mm` PR #42](https://github.com/joelanders/gearmulator-md-mm/pull/42)
   corrects the MD mode labels, models the codec input connection to both MD
   DSPs, adds firmware regressions, and advances the DSP submodule.

The corresponding local commits are `ac2351a4` in the DSP repository and
`654fdf53` plus `069c900b` in the main repository.

## Conclusions and confidence

Confidence is an engineering judgment based on the evidence below, not a claim
that all MD/MM audio behavior is covered.

| Area | Confidence | Basis |
| --- | ---: | --- |
| Classic/Extended mode presentation | 98% | Real MD OS 1.63 status messages identify the raw values unambiguously; synthetic and firmware-backed tests agree. |
| DSP `MERGE` implementation and RAM sample packing | 97% | The instruction sequence and corrupt packed words were traced directly; interpreter/JIT unit tests and real firmware playback pass. |
| MD codec-input delivery to RAM-R | 90% | Both DSPs' ESSI/DMA setup was traced, external recording now works, and queue telemetry remains clean in the measured tests. Exact hardware timing and long-duration behavior remain to be checked. |
| Combined change | 92-94% | Fresh Release builds and all available automated MD tests pass. The remaining uncertainty is primarily hardware, old-firmware, long-duration, and MM-fixture coverage. |

## Classic and Extended mode

MD OS 1.63 reports the mode through SysEx status `0x20` and the front-panel mode
LED bank. The observations are:

- raw value/bit 0 is **Classic**;
- raw value/bit 1 is **Extended**; and
- the firmware sequencer already implements the hardware behavior.

The emulator attached the UI names to the opposite LED bits. Consequently, the
firmware behavior appeared reversed when described using the displayed mode.
The fix swaps the semantic names in `mdfrontpanel.h`; it does not patch or
override the firmware sequencer.

This is high confidence because the correction is a direct mapping with two
possible values. A physical smoke test should nevertheless confirm that the
mode named Extended changes kits with patterns and the mode named Classic does
not.

## RAM noise and `RATE` tones

### Missing `MERGE` instruction

MD OS 1.63 packs two 12-bit RAM samples on DSP2 with this sequence:

```text
move  y:(r4)+,a
move  y:(r4)+,b
merge a1,b
move  b1,x:(r0)+
```

`MERGE` was decoded but effectively unimplemented in both the interpreter and
JIT. The second sample therefore remained in the low half while the high sample
was absent. Recorded silence repeatedly became `0x000800`; RAM-P decoded that as
a strong alternating signal, with a measured sustained peak of `0.02363`, RMS
of `0.020316`, and adjacent-sample correlation of `-0.995868`. Moving `RATE`
changed the pitch of this corruption, which explains why the report sounded
like a rate-control or firmware problem.

The corrected instruction produces `{S[11:0], D[35:24]}` in destination bits
`47:24`, preserves the other accumulator fields, sets `N` from result bit 23,
sets `Z` when the 24-bit result is zero, and clears `V`. Correctly packed silence
is `0x800800`.

Unit coverage exercises all six source registers, both destinations, aliased
operands, asymmetric half words, preserved fields, and condition flags in the
interpreter and JIT. An ADD/MERGE sequence checks that lazy E/U flags from the
preceding arithmetic instruction are materialized before MERGE updates N/Z/V.
The opcode analysis also declares the destination as read/write. The
implementation follows the standard arithmetic mode used by the traced MD path.

### DSP2 received silence instead of the codec input

Correct sample packing exposed a second problem: external RAM-R recordings were
actually silent. The earlier malformed samples had made the old firmware test
mistake corruption for recorded audio.

Firmware tracing established the MD input topology:

- DSP1 receives codec ADC Input A/B into its ESSI1 DMA ring and uses that ring
  for input metering.
- DSP1 sends the processed main mix to DSP2 over ESSI0.
- DSP2 independently configures ESSI1 receive DMA for the codec ADC input.
- DSP2 owns the UW RAM and applies RAM-R's external-input level/balance controls
  to that ESSI1 input; the main-mix controls use the separate ESSI0 stream.

The emulator previously connected DSP2's ESSI1 receiver to a silence producer.
For Machinedrum, each host input frame is now copied into an independent queue
for each DSP. Independent queues are required because the two emulated DSP
schedulers consume the shared physical ADC stream separately; a single queue
would allow one DSP to steal the other one's frame.

Monomachine retains its established DSP2 silence producer. Its second queue is
neither primed nor appended, avoiding stale samples and false overflow reports.

## Validation evidence

A clean Release build outside the worktree passed the following gates on
2026-09-05:

- DSP assembler tests: 280/280;
- DSP interpreter unit tests;
- DSP JIT unit tests;
- DSP JIT optimizer tests;
- `mdLibTest`, including Classic/Extended LED mapping;
- `mdAudioQueueTest`;
- `mdRamAudioOracleTest`, including rejected silence, transients, alternating
  noise, wrong-input waveforms, NaN, and infinity;
- `mdUwFirmwareTest` with MD UW OS 1.63; and
- `mdAudioFirmwareTest` with MD UW OS 1.63.

The firmware-backed RAM test verifies:

- RAM-R at `RATE=64` records silence that stays below `0.0001` during sustained
  RAM-P playback;
- distinct saw signals on Input A/B at `RATE=127` change UW sample memory;
- `IBAL` at each extreme records the selected input: two consecutive 512-sample
  playback windows correlate above 0.99 with that input (required: 0.90), while
  correlation with the other input stays below 0.20 (maximum allowed: 0.35);
- both output channels have sustained RMS around 0.0574-0.0576 and peaks of
  `0.103503` / `0.108209` for the A/B captures (ROM peak: `0.276559`); and
- each 16,384-frame capture has zero host-input queue underflows and overflows.

The audio oracle fits phase and gain rather than requiring a fixed firmware
latency. Its test translation unit disables fast-math so NaN/Inf rejection
remains meaningful in Release builds. This is a waveform-fidelity regression,
not a physical-hardware latency or gain calibration.

The review follow-up also passed the DSP assembler, interpreter, JIT, and
optimizer suites in both native ARM64 and x86-64 (Rosetta) Release builds.
Focused DMA tests issue single word requests and inspect 24-bit source-address
wrapping for in-line increment and positive/negative DOR-A and DOR-B offsets.
As negative controls, temporarily restoring MERGE's old cache reset failed the
new E/U preservation test, and removing the DMA mask failed the address-wrap
test. Both fixes were restored before final validation.

The randomized audio test passed with the Legacy, MameHq, and MameLofi
resamplers at 44.1, 48, and 96 kHz, including hostile host block sizes and
randomized DSP scheduling. Queue telemetry now exposes each DSP separately;
aggregate counts sum receiver-frame events for both underflow and overflow.
The Monomachine's unused second receiver remains at zero. Both commit ranges
passed whitespace checks after validation.

## Remaining reservations

### Physical timing

The data topology is supported by the firmware, but a physical Machinedrum is
still the authority for the exact frame latency and phase relationship seen by
the two DSP receivers. The independent queues could reveal slow drift only in a
recording substantially longer than the automated capture.

### RAM machine coverage

The regression deliberately isolates silence and an external saw input. It does
not exhaustively cover:

- RAM-P2 and interaction between RAM-P1/RAM-P2;
- combined main-mix and external-input recording;
- all `ILEV`, `IBAL`, `MLEV`, and `MBAL` extremes;
- intermediate and extreme `RATE` values beyond the two regression points;
- record retriggering, loop wrap, and boundary lengths; or
- host suspend/resume, sample-rate changes, or state restoration while input
  queues contain data.

These are the most likely places for remaining MD RAM behavior bugs. The
original pitched-tone mechanism should be gone at every rate because packing is
now independent of the playback rate, but interpolation and boundary behavior
still come from firmware paths not fully exercised by the regression.

### Firmware versions and Monomachine

Only MD UW OS 1.63 was available for real-firmware testing. Older Machinedrum
firmware might configure the serial links differently. No Monomachine firmware
fixture was available locally. The main-repository change explicitly preserves
the existing MM DSP2 producer behavior, but the generic DSP instruction change
would benefit from a real MM boot/audio regression.

### Generic DSP emulator scope

The DSP56300 manual defines separate behavior in Sixteen-bit Arithmetic mode
(distinct from the S0/S1 scaling modes). The emulator does not model that mode
globally, and the traced MD `MERGE` path does not enable it. If another firmware
executes `MERGE` in Sixteen-bit Arithmetic mode, that should be implemented and
tested as a separate emulator change.

The DMA audit also found generic conformance debt outside the observed MD/MM
paths: Mode-E count extraction, DTD state, live DCO visibility, DE-triggered 3D
timing, and dual-3D addressing. Only the previously lost 24-bit target-address
mask was restored here. The other items should be tracked and tested separately
rather than folded into this audio fix.

## Recommended hardware smoke matrix

Before calling the behavior fully hardware-validated, run at least the following
on the same pattern and source material in the emulator and on a Machinedrum UW:

| Test | Controls | Pass condition |
| --- | --- | --- |
| Mode semantics | Switch Classic/Extended while changing patterns assigned to different kits | Kits follow patterns only while the UI says Extended. |
| Silent RAM record | RAM-R external-only, several `RATE` values | RAM-P output has no pitched tone or structured noise floor. |
| External RAM record | Sweep `ILEV`, `IBAL`, and `RATE`; play through RAM-P1 and RAM-P2 | Level, balance, and pitch follow hardware without corruption. |
| Main-mix RAM record | Sweep `MLEV` and `MBAL`, including mixed external/main input | Recorded balance and gain agree with hardware. |
| Boundary behavior | Short/maximum loops, retrigger, and loop wrap | No stale burst, discontinuity beyond hardware behavior, or address wrap fault. |
| Long capture | Record/play for several minutes at 44.1, 48, and 96 kHz with varied host block sizes | No queue drift, dropouts, changing noise floor, or growing latency. |
| Host lifecycle | Suspend/resume audio, change sample rate, save/restore state | Queues recover without stale audio, underflow storms, or crashes. |

Record the hardware OS version, host/DAW, plugin format, sample rate, block size,
RAM machine parameters, and whether input-queue telemetry changed. A failure in
the long-capture or lifecycle rows is more likely to implicate queue scheduling;
a failure at particular RAM parameters is more likely to be a firmware-path,
interpolation, or buffer-boundary emulation issue.
