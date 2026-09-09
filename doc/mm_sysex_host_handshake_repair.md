# Monomachine DigiPRO: host-handshake repair

2026-09-09. Follow-up to the [release-history bisect](mm_sysex_release_regression.md).
Work is confined to the existing `gearmulator-md-mm-audio-tests-review` worktree.

The parent starts from `33983cd0`. Tested dependency changes are committed
locally on `fix/mm-host-command-ack-20260909` in each existing submodule:
DSP `458ec253` (parent `c9a154a3`) and MCU `f365043` (parent `1ae33bff`).
These were local repair commits at the investigation checkpoint. They are now
published in the PRs below, but are not merged release pins.

## PR preparation checkpoint

2026-09-09. The repair is published for review in the user's forks:

- Parent [Gearmulator #60](https://github.com/joelanders/gearmulator-md-mm/pull/60),
  draft, targeting `release/md-mm-alpha`.
- [DSP #17](https://github.com/joelanders/dsp56300-md-mm/pull/17), targeting
  `release/md-mm-public-alpha-20260826`.
- [MCU #6](https://github.com/joelanders/mc68k-md-mm/pull/6), targeting
  `release/md-mm-public-alpha-20260826`.

Fresh remote checks found parent release `5d3b2597` and DSP release `c9a154a3`.
MCU release `4a6d0d1` contains the previously tested `1ae33bff` baseline with an
identical source tree. Neither dependency PR requires the older deferred
firmware-hook/lifecycle cleanup PRs. No PR was merged during preparation.

The production/test code remains parent `749f6124` with DSP `458ec253` and
MCU `f365043b`; this publication update changes documentation only. A fresh
Release rebuild and selected CTest run passed all 19 tests with no failures
or skips in 63.17 seconds, including sender/SDS, flash/state, and the two
ROM-dependent host/scheduler tests. All seven Python runner tests passed again.
Local evidence is `temp/mm-pr-preflight-20260909.log`. This recheck did not
repeat the 12 complete workflow runs or rebuild the JUCE processor harness.

Before merging the parent: merge both dependencies first, update its gitlinks
to their actual resulting release revisions (preserving other merged fixes),
and rebuild/retest that exact combination. Required CI, the final processor
lifecycle regression, packaged plug-in builds and host behavior remain gates;
the current CI workflows do not run every new SysEx harness automatically.
Published branches and local passes are not evidence of CI success. Consult
the live PRs for subsequent review/check/merge status.

## Diagnosis

Deferring received words exposed other host-interface operations that still
reported results from a DSP running ahead of CPU time. This was not another
SysEx-header rejection, nor a reason to publish waveform data prematurely.

The first captured import-handshake divergence is around command vector `$0e`
on DSP2. The host supplies `$0004fe`, writes CVR, supplies `$000001`, then reads
the receive registers consecutively. The existing pre-command drain executes
400,012 DSP cycles with CPU time fixed. The original CVR implementation clears
HC immediately after the write callback, even though the DSP has not yet
accepted that command at CPU-visible machine time.

Selected observations from `temp/mm-repair-command-baseline`:

| Event | CPU cycle | DSP cycle / receive deadline |
| --- | ---: | ---: |
| Command begins | 1,047,757,463 | DSP 2,660,768,897 |
| Command injected after synchronous drain | 1,047,757,463 | DSP 2,661,168,909 |
| Host consumes the first notification word | 1,047,757,472 | Second word `$000401` is reserved |
| Subsequent reads find no available word | 1,047,757,474 and 476 | Second word is not due until 1,047,911,306 |
| Second word finally becomes visible | 1,047,911,307 | Delivered separately to the interrupt handler |

Thus a notification is split across the firmware's drain sequence and a later
interrupt. The trace identifies an incorrect handshake preceding the corrupted
import; it is not a claim to have decoded every firmware consequence of that
orphaned word.

The hardware contract supports correcting acknowledgement rather than adding a
delay: HC/HCP clear when the DSP services the command request, RXDF indicates
received data, and reading the final receive byte clears RXDF. See the
[NXP DSP56303 User's Manual](https://www.nxp.com/docs/en/reference-manual/DSP56303UM.pdf),
sections 6.6.2, 6.7.2, 6.7.3 and 6.7.5. Our scheduler must apply the same
machine-time visibility rule to acknowledgement and received data.

## Retained repair

1. DSP HI08 records the actual command-acceptance cycle. Its pending query
   includes a queued command, but excludes a command already accepted whose
   handler has not yet returned. The acceptance timestamp is not overwritten
   by subsequent DSP execution or host polling.
2. The CPU-facing HI08 supports a CVR-read callback, including word reads of
   ICR/CVR. The default preserves other hosts' existing behavior. Writes do
   not reenter this read callback. The MM bridge keeps HC asserted until the
   command is accepted and CPU time reaches that acceptance timestamp.
3. An ISR callback that publishes a due receive word returns the new RXDF
   state, not the snapshot from before publication.
4. An empty-read callback does not run the MM DSP speculatively when a reply
   is already reserved or readable. CPU time still determines visibility.

Deferred receive-word delivery, native receive capacity, board clocks,
SysEx pacing, firmware data, and complete-content comparisons remain intact.
No firmware-PC conditional, guessed delay, FIFO enlargement, immediate-data
fallback, private task-list change, or diagnostic logging remains in production.
The unrelated deferred firmware-hook cleanup is not part of this change.

## Why intermediate passes were insufficient

| Experiment | Outcome |
| --- | --- |
| Initialize receive state as specified by INIT/RREQ | Same bank failure; experiment removed |
| Stop speculative empty-read execution alone | Same bank failure |
| Remove pre-command receive draining | Bank fails from slot 0; experiment removed |
| Acknowledge using the first host observation's DSP time | Bank and mixed pass, but this is not the exact acceptance timestamp; replaced |
| Exact acceptance timestamp alone | Mixed passes; bank still fails at slot 23 |
| Exact timestamp plus empty-read guard | Same slot-23 failure: three final bytes remain erased |
| Also return current RXDF after publication | Complete bank passes, including reconstruction and audio |

The content oracle now reports all mismatching slots instead of stopping at the
first one. It still fails if any expected byte is missing. This confirmed that
the intermediate slot-23 result was a real failure, not permission to ignore
padding or reduce the comparison.

## Focused regression tests

`mdHostRxFirmwareTest` adds controlled host-interface sequences for both DSPs:

- Pending versus accepted versus still-in-handler command states.
- An unrelated interrupt cannot acknowledge the command.
- Exact acceptance deadline, 64-bit acceptance metadata, and preservation of
  its timestamp after subsequent DSP progress.
- Byte and word CVR reads; a queued command requires its own acknowledgement.
- Reconfiguring command arbitration clears the timestamp.
- Empty reads cannot run past an already-reserved reply.
- A status read that itself publishes data must immediately report RXDF.

These use synthetic instruction/register state initialized with the supported
ROM. The workflow tests separately execute real firmware. Existing long-runtime
conversion, receive-latch capacity, overwrite, IRQ routing and scheduler-boundary
tests are retained.

Each new bridge correction has an independently exercised negative control:

| Removed correction | Required test failure |
| --- | --- |
| CVR acknowledgement callback | Command acknowledged before DSP acceptance |
| RXDF refresh | ISR returns snapshot from before publication callback |
| Reserved-reply empty-read guard | Empty read speculatively advances the DSP |

All controls fail normally under CTest's external deadline. Each is restored
before final verification. The MCU's ROM-free receive test also verifies CVR
callback dispatch, preserved vector bits, byte/word access and default behavior.

## Verification and reproduction

Release arm64, ordinary `-O3`, ThinLTO/PGO disabled; stock MM OS 1.32b and the
same patch-RAM/public-bank fixtures as the bisect. Fixture and executable hashes,
commands, exits, durations and runtime variation are retained in runner reports.

The clean selected core/timing suite passes 15 tests with no skips, including
both ROM-dependent host/scheduler tests. Seven ROM-free runner tests pass.
Subsequent queued-command coverage also passes the host integration test.
The final six-test smoke run also includes the 64-bit acknowledgement cases;
it passes with no skips after removing the temporary diagnostic CMake target.

The bank/mixed matrix verifies every supplied waveform/kit payload, booted saved
state reconstruction, and four selected waveform/audio correlations before and
after reconstruction. `MM_SYSEX_BLOCK_PROFILE` varies boot/panel/transfer
advancement and audio callback chunks; simulated panel settling durations and
content/audio thresholds are unchanged. Unset retains the previous harness
behavior. Explicit profiles are `32`, `64`, `1024` and `irregular` (1, 17, 63,
128, 511, 1024, 3). `GEARMULATOR_MDMM_BOUNDED_JIT=0` selects the existing
individual-block dispatcher instead of the default bounded dispatcher.

```sh
cmake --build temp/build-sysex-release --target \
  mdHostRxFirmwareTest mdIdleSchedulerFirmwareTest mmSysexWorkflowTest mdLibTest -j 4
GEARMULATOR_MM_FIRMWARE_BIN=/path/MM-1.32b.bin \
  ctest --test-dir temp/build-sysex-release --output-on-failure \
  -R '^(mdLibTests|mdHostRxFirmwareTest|mdIdleSchedulerFirmwareTest|mmDigiProImportAudioOracleTest|mmSysexBlockProfile-.*)$'

MM_SYSEX_BLOCK_PROFILE=irregular GEARMULATOR_MDMM_BOUNDED_JIT=0 \
python3 source/elektron/md/mdLibTest/runSysexConfidence.py \
  --suite workflow --case 'workflow-2 TRI--INV' --case workflow-mixed \
  --timeout-seconds 300 \
  --bin-dir temp/build-sysex-release/source/elektron/md/mdLibTest \
  --mm-rom /path/MM-1.32b.bin --mm-patch /path/mm-patch.bin \
  --corpus /path/sysex-web-corpus-20260908 --output temp/new-recheck
```

Local ignored evidence is under `temp/mm-repair-*`: bounded host traces,
intermediate controls, negative-test logs, clean workflow reports, and modulation
captures. One early diagnostic disassembler returned zero instruction length,
causing repeated logging; that process was stopped and only its generated log
was removed. Its interrupted run is excluded from evidence. No firmware,
waveform bank, flash export, audio capture or executable is committed.

Final clean workflow results (all include complete contents, reconstruction and
audio; elapsed times are diagnostic runs under concurrent load, not benchmarks):

| Configuration | TRI bank | Mixed workflow |
| --- | --- | --- |
| Default | Pass, 86.915 s | Pass, 105.065 s |
| 32-frame blocks | Pass, 86.097 s | Pass, 103.450 s |
| 1,024-frame blocks | Pass, 100.360 s | Pass, 106.498 s |
| Irregular blocks | Pass, 99.902 s | Pass, 106.147 s |
| Irregular blocks, individual-block DSP dispatcher | Pass, 92.844 s | Pass, 103.231 s |

Additional full 64-wave public banks `1 SINE-EXT.syx` and `5 FM---INV.syx` pass
contents/reconstruction/audio (92.526 and 86.390 s). The MD sample-import
regression passes all 5,201 sample words, state encoding and playback correlation
0.990398, with no retries. This exercises the shared host-register changes on
Machinedrum OS 1.63 with its existing factory cache.

The existing six-voice modulation harness captures the reference pitch-LFO case
and the three-LFO pitch/volume/pan case, nine seconds each including release.
The unchanged analyzer passes both cases, verifies all five background voices
individually and together, and checks three repeated-note curves per case.
Maximum aligned pitch-curve errors are 0.189963% and 0.190080% of the feature
span. Across 12,368 sampled active-LFO states there are zero clock stalls,
negative increments or invalid phases. These are repeatability/glitch checks,
not physical-hardware accuracy measurements or worst-case FM/effects stress.

That campaign reuses the durable
`notes/sol-high/modulation-followup-20260905/{run.py,analyze.py,diagnostic/mdModulationLoadTrace.cpp}`
from the existing recovery worktree. The local harness copy is under
`temp/mm-repair-modulation`: its only C++ adaptations remove the obsolete,
disabled `setMmCleanGndSin` diagnostic option and redirect three fixed temporary
screenshots into this run's output prefix. The runner points at this worktree,
its current build, and isolated fixture/output paths. Its case definitions,
panel/MIDI setup, content/state observations and the analyzer's thresholds are
unchanged. No application firmware-private write is added. A temporary CMake
target builds that local copy against the current `mdLib`; that target is
removed after testing. Capture metadata records executable, harness and fixture
hashes. The analyzer emits a harmless font-cache permission warning but exits 0;
both complete captures and all checks are required for success.

Useful artifact names: `mm-repair-final-regressions.log`,
`mm-repair-queued-test.log`, `mm-repair-runner-tests.log`,
`mm-repair-negative-{cvr,isr,guard}.log`, the five
`mm-repair-clean-*/report.json` workflow reports,
`mm-repair-extra-banks/report.json`, `mm-repair-md-sds.log`, and
`mm-repair-modulation/{capture,results}`.

## Release limits

This is a targeted host-interface integration repair, not complete bus/cache
cycle accuracy. The existing synchronous drain/arbitration machinery is not
redesigned. Hardware parity, other firmware versions/platforms, the tester's
exact file/build, and actual DAW callback/UI behavior remain separate acceptance
work. Local tests do not establish CI or release-binary success. Dependencies
must be published/reviewed and integrated in dependency-first order, then the
parent pins updated to those release revisions and the combination retested.
