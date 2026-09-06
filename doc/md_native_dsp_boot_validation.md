# Native DSP boot replies: independent extraction

Prepared 2026-09-06 from release `7cd7afa0`, which already includes the recent
MM timing and project-restore changes. This is the boot-reply slice of cleanup
PR #43, originally investigated in `71e492dc`, with the repeated panel-progress
regression from `95930077`. It does not import the large cleanup stack.

## Scope

Remove the DSP2 query interception, model-specific synthesized reply, and
command-vector-specific deferral/arming state. Commands use the existing
host-command path; data words use the existing paced receive path, allowing
the supplied firmware to execute and produce its own response. No alternate
query signature, private-memory access, hardcoded reply or timing constant is
introduced.

The DSP and MCU submodule pins remain the release versions (`8c919d2b` and
`1ae33bff`). This PR does not depend on the independent DMA-status extraction.
The MM audio correction, private parameter guard, waveform injection and MD
panel-task workaround are unchanged and are not claimed as remediated here.

## Observable regression

`mmBootFirmwareTest` loads a user-supplied, fingerprint-checked MM OS 1.32b
image and starts a fresh machine. It requires:

- both DSPs booted and panel/MIDI readiness after 20 seconds of emulated time;
- a firmware-generated kit-status reply through the emulated MIDI UART;
- changed LCD contents on each of three tempo-menu entry/exit cycles.

The test uses ordinary external panel/MIDI operations, not firmware-memory
patches. Boot or finite silence alone cannot satisfy it. With no fixture it
returns 77, registered as a CTest skip rather than a passing firmware check.
Building `mdLibTest` also builds the new executable; it can be selected directly
by its CTest name. No firmware image or extracted payload is committed.

## Validation record

With only the new regression added and the release's interception still intact,
fresh ARM64 and x86-64/Rosetta Release builds passed MM boot/kit/panel and the
existing MD UW/RAM test. MD ROM peak was `0.276559`; recorded-input correlations
were `0.990251` through `0.991804` on both architectures.

After removal, both fresh builds passed all 11 selected tests, with no skipped
tests in the supplied-fixture runs:

| Gate | ARM64 | x86-64 / Rosetta |
| --- | --- | --- |
| MM boot, kit reply and repeated panel interaction | Pass, 17.59 s | Pass, 27.65 s |
| MD UW/RAM/mode firmware regression | Pass, 49.02 s | Pass, 77.85 s |
| MD/MM randomized scheduler/resampler audio soak | Pass, 12.37 s | Pass, 21.13 s |
| Runtime, audio queue, RAM oracle and five required MCU/host-timing fixtures | All pass | All pass |
| Complete selected run | 11/11, 80.46 s | 11/11, 129.54 s |

A separate no-fixture invocation correctly reported the MM test as skipped
(return 77). That skip is not included in the 11/11 result. Both dependency
worktrees were clean at the post-removal runs; the temporary DMA-only product
comparison used while preparing the other PR had already been removed.
Results from the original large cleanup branch are not substituted for these
extracted-branch checks. Interpreter firmware tests and physical-device
comparisons were not run for this slice.

## Running the firmware gates

Configure the normal headless Elektron Release build for the chosen architecture,
then build and run the focused targets:

```sh
cmake --build build --target mmBootFirmwareTest mdUwFirmwareTest mdAudioFirmwareTest --parallel 2
export GEARMULATOR_MM_FIRMWARE_BIN=/path/to/elektron_sfx6-60_os1.32b.bin
export GEARMULATOR_MD_FIRMWARE_BIN=/path/to/elektron_sps1-1uw_os1.63.bin
ctest --test-dir build -R '^(mmBootFirmwareTest|mdUwFirmwareTest|mdAudioFirmwareTest)$' --output-on-failure
```

Also run `mdLibTests`, `mdAudioQueueTest` and `mdRamAudioOracleTest` with their
required build targets/CTest fixtures. Missing firmware skips are not evidence
that the supplied-image gates passed.

## Confidence and limits

This is an empirical hook-removal test for the supported fixtures: normal
emulation must preserve the observed startup, panel and MD RAM behavior.
It does not establish the hook's historical necessity, precise physical
host-interface timing, all MM synthesis settings, interpreter parity, or full
reset/state-restoration behavior. The older emulator is a regression baseline,
not an independent physical-hardware oracle. No legal or clean-room claim is
made by this removal.
