# DigiPRO release regression: deferred host-word delivery

2026-09-08, macOS arm64 Release. Follow-up to the
[SysEx lifecycle investigation](md_mm_sysex_lifecycle.md).

The first failing backend change is `b07ae365ac75835cdadc169ab2166738f61eb8a7`,
**Preserve DSP production time when publishing Monomachine host words**.
Its immediate parent, `eb89cd49f46dad611762dda753812699b03d5c1d`, passes the
same bank and mixed workflow. The change entered the release through PR #44,
merge `88646381`, before alpha.10. It is not a regression introduced by the
later native-waveform/native-audio removals or the latest UART merge.

This isolates the faulty integration, not a shippable repair. A current-release
control that bypasses only deferred host delivery makes both failing workflows
pass, but breaks the existing host-timing regression. Publishing words early
is not an acceptable production fix.

## Fixed experiment inputs and scope

All comparisons reuse `gearmulator-md-mm-audio-tests-review`; the previously
passing `gearmulator-md-mm-sysex-sender-v2` is rebuilt as a control and remains
unchanged. No additional project checkout/worktree is created. The active
branch remains `fix/md-mm-sysex-lifecycle-20260908`, based on release `5d3b2597`
with the scoped SysEx commits and lifecycle work through `1c134e16`.

Historical experiments transplant the relevant `mdLib` backend source to its
historical revision and select that revision's DSP/MCU pins **in the existing
submodule checkouts**. The new SysEx sender, shared NOR programming fix,
complete-content oracle, and workflow harness remain constant. These are
backend comparisons with the new import feature, not claims to have tested
verbatim historical plug-in binaries. Diagnostic observer/API additions remain
in the harness; they do not modify firmware storage or scheduler decisions.

The standalone target is `mmSysexWorkflowTest`, built with Apple Clang,
Release `-O3`, arm64. ThinLTO/PGO are disabled. Historical scheduler-specific
flags follow the tested source revision. The pre-PR-44 MCU lacks two later
CTest targets; their build dependency names are temporarily omitted to configure
that historical backend. Those CMake edits are restored afterward. No test
oracle, input bank, acceptance threshold, or receive-menu sequence is weakened.

Fixtures are stock MM OS 1.32b, the same factory patch-RAM fixture as the
previous investigation, and public `2 TRI--INV.syx`:

```
bank SHA-256: e332f7e7a5dfe035dc577387fc790392f586153fad12783cecba82abbab5fa38
```

The bank has 64 waveform messages. The mixed fixture contains a distinct kit
at slot 62, waveforms 0/1/45/63, and a distinct final kit at slot 63. Passing
means complete decoded waveform contents, kit readback where applicable,
booted project reconstruction, and waveform/audio correlation at four selected
slots both before and after reconstruction. A transfer reaching its byte count
alone cannot pass.

## Historical comparisons

| Backend/source | DSP pin | MCU pin | Result |
| --- | --- | --- | --- |
| Earlier SysEx integration `e3d4d397` | `8e4708b0` | `62dc26cd` | Rebuilt bank test passes, including reload/audio |
| Release before PR #44, `352ddbb6` | `363d3fc0` | `b8c774a5` | Bank passes all 64 waves and reload/audio |
| Pre-PR-44 main/DSP plus newer MCU only | `363d3fc0` | `1ae33bff` | Boot fails; not an import result |
| `eb89cd49`, first coordinated timing/latch change | `363d3fc0` | `4182ceb0` | Bank and mixed workflow both pass, including reload/audio |
| Immediate next commit `b07ae365`, deferred publication | `363d3fc0` | `4182ceb0` | Bank halts the emulated DSP; mixed workflow returns failure for wave contents |
| Alpha.10 backend `7cd7afa0` | `8c919d2b` | `1ae33bff` | Bank contents fail |
| Current release, only UART restored to pre-#50 behavior | `c9a154a3` | `1ae33bff` | Bank contents fail; flash byte-identical to current baseline |
| Current release `5d3b2597` plus SysEx/lifecycle work | `c9a154a3` | `1ae33bff` | Bank contents and mixed final-kit checks fail |

The two UART variants' complete exported flash images have identical SHA-256:
`123fff5e9515d96eec0a26d6c47f5c3db44067c71723adfa40d13421a0b3dd41`.
This control rules out that UART merge for the bank corruption, not every
possible UART-related workflow issue.

`b07ae365^` is exactly `eb89cd49`; DSP and MCU pins are identical across this
adjacent pair. The changed backend path adds timestamp capture, a reserved
receive word, delayed publication until host time reaches the word's deadline,
and scheduler wake/pump checks for that reservation.

The `b07ae365` bank process reports an invalid DSP execution address and halts
inside emulation. Its inner test deadline cannot stop a blocked `advance()`;
the exact diagnostic process was identified and terminated, with exit 143.
This is recorded as an interrupted failing diagnostic, **not** an ordinary
test exit or a pass. Its separate mixed test completes normally with exit 1
in 76.832 seconds: both kits read back correctly, but waveform 1 is corrupt.
Later release changes alter the symptom without eliminating the regression.

The old MCU SHA `4182ceb0` was unavailable from the remote (`not our ref`).
It was recovered read-only from an existing local MCU object database under
the `gearmulator-md-mm-pr25-fix` worktree metadata. It is retained locally at
MCU ref `refs/diagnostics/mm-sysex-bisect-20260908`. Its source tree is identical
to the reachable MCU commit `fa387ff3`:

```
4182ceb0^{tree} = fa387ff3^{tree}
               = 8b2bf3f7dc402a04ea55a781c17a2b495d241f1b
```

No substitute core was silently used for the adjacent-commit comparison.

## Current-release causal control

After restoring every historical source overlay and both release pins, remove
only the Monomachine-specific deferred branch from
`Dsp::hdiTransferDSPtoUC` in `mddsp.cpp`, allowing the existing immediate
transfer path below it to serve MM too. Leave the sender, UART, CPU/DSP clocks,
receive capacity, request threshold, native audio/waveform behavior, state
codec, and test oracle unchanged. This is an experimental negative control,
not a proposed shipping edit.

| Current-release configuration | TRI bank workflow | Mixed workflow | Existing host-timing gate |
| --- | --- | --- | --- |
| Normal deferred delivery | Fails complete contents | Fails final kit | Passes |
| Immediate-delivery control | Passes, 82.149 s | Passes, 92.607 s | Fails |
| Deferred delivery restored | Fails again | Fails again | Passes again |

Both passing workflows include booted reload and audio comparison. In the
immediate mixed control both complete kit payloads and all four waveform
payloads also match after reconstruction. This distinguishes the observed
release failure from merely pressing Exit at the wrong time in the driver.

The immediate control fails `mdHostRxFirmwareTest` with:

```
HOTX replacement changed native capacity or published the reserved word
```

After restoring normal code, that test and `mdHostRxTimingTest` pass. The
existing timing tests intentionally reject premature visibility. Their success
does not establish that a real firmware import sees a correct multiword stream.
Conversely, passing DigiPRO after publishing early does not establish correct
modulation timing. **Both obligations must hold in the same implementation.**

No immediate-publication fallback, guessed inter-message delay, larger receive
FIFO, firmware-PC condition, private task-list edit, or relaxed data comparison
is retained. All production source and both submodule pins are restored.

## What this establishes, and what it does not

This is a backend timing/visibility integration regression, not rejection of a
SysEx header or a parser-only defect. It connects the earlier historical
boundary to the current release using a one-path current-code control. We do
not need to hypothesize a broad application rewrite or merge the deferred
firmware-hook branch to reproduce it.

It does **not** yet identify the correct physical bus/CPU timing repair, prove
which specific read first loses a word, or prove that the deadline arithmetic
itself is wrong. The timestamp conversion unit tests remain valuable. There
is no claim that the alpha tester's exact file/build or a real hardware trace
has been reproduced.

The next repair should instrument the emulated host boundary around the first
divergence: DSP word production and reservation, host-visible publication,
RXDF/HREQ, status/data reads, empty-read handling, and the CPU/DSP time used by
those operations. Determine whether synchronous reads/callbacks/clock advancement
remain coherent once data visibility is deferred. Use bus/register observations,
not firmware-private layouts. Any proposed correction must retain the existing
host-timing gates and simultaneously pass the complete bank/mixed/reload/audio
tests. Then rerun modulation/glitch tests and the other public banks before
claiming shipping readiness.

## Repeatable focused runner

`runSysexConfidence.py` now accepts repeated exact `--case` selectors and an
external `--timeout-seconds` deadline. Unknown case names and nonpositive
deadlines fail rather than silently running nothing. The report retains the
suite, selection, deadline, executable/fixture hashes, commands, exits, and
durations. The external deadline also bounds a hung emulation call. Six
ROM-free runner tests pass, including subset selection and timeout propagation.

For either a normal build or a separately saved diagnostic executable:

```sh
python3 source/elektron/md/mdLibTest/runSysexConfidence.py \
  --suite workflow \
  --case 'workflow-2 TRI--INV' --case workflow-mixed \
  --timeout-seconds 300 \
  --bin-dir temp/build-sysex-release/source/elektron/md/mdLibTest \
  --mm-rom /absolute/path/MM-1.32b.bin \
  --mm-patch /absolute/path/mm-factory-live3-be.bin \
  --corpus /absolute/path/sysex-web-corpus-20260908 \
  --output temp/mm-release-recheck-new
```

The corpus layout is the existing documented workflow corpus, including its
three selected public banks. Output reports must not already exist. These
current-baseline cases are expected to fail until the backend is repaired;
do not reclassify their failures as successful imports.

Local, ignored artifacts under the reused worktree's `temp/`:

- `mm-bisect-pre-uart-*`, `mm-bisect-alpha10-*`, `mm-bisect-pre44-*`,
  `mm-bisect-core-only-*`, `mm-bisect-eb89-*`, `mm-bisect-b07-*`.
- `mm-bisect-release-immediate-report/report.json` and its logs.
- `mm-bisect-release-restored-report/report.json` and its logs.
- `mm-bisect-immediate-timing.log`, `mm-bisect-restored-timing.log`,
  `mm-bisect-restored-timing-unit.log`, `mm-bisect-runner-tests.log`.
- Saved adjacent-revision executables `mm-bisect-eb89-executable` and
  `mm-bisect-b07-executable`, with SHA-256 respectively
  `2d1556f5c4a9e8c626b30995ca9a10473653f0a507534db4827cbda0e3c046a7`
  and `c8720d1e21ef28da5c96f0a54d7e5f4876ded1f5f37dc83a3e4707e65411f5a6`.

The earlier branch's rebuilt control log is
`gearmulator-md-mm-sysex-sender-v2/temp/mm-sysex-bisect-old-bank.log`.
No firmware, patch RAM, bank, recording, flash export, or executable is committed.
