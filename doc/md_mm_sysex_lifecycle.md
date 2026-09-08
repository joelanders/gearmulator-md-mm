# MD/MM SysEx: release-baseline architecture and lifecycle investigation

2026-09-08, macOS arm64. This is a bounded architecture/root-cause pass, not
certification that every import works or that the alpha tester's exact file
has been reproduced. No ROM, cache, downloaded bank, or firmware-private
disassembly is committed. No new firmware-private hook or shipping delay is
introduced.

## Shipping baseline and branch provenance

Live, read-only GitHub checks identified `release/md-mm-alpha` as the default
branch at `5d3b2597b9605fe601c5c7d935e0ac177e7bdf3d`. Latest published prerelease
was `mdmm-v0.1.0-alpha.10`, published 2026-09-06, targeting
`7cd7afa0a7fab689a4cf628cba96cd8243d0aed3`. The release branch contains subsequent
fixes and is the integration baseline used here; it is not identical to the
published alpha. Sources: repository/branches/releases responses from
`gh api repos/joelanders/gearmulator-md-mm` (and its corresponding endpoints).

Previous SysEx implementation/testing was on `refactor/md-mm-firmware-hooks`,
at `e3d4d397`, with an older, deferred emulator-cleanup integration baseline.
Its passing results do not establish correctness on the release branch.
That worktree and branch are preserved unchanged.

A clean stale worktree, `gearmulator-md-mm-audio-tests-review`, was reused.
Its former `test/mm-audio-removal-regressions` branch is preserved. New local
branch `fix/md-mm-sysex-lifecycle-20260908` starts at release `5d3b2597` and
extracts only the four scoped SysEx commits:

| Previous commit | Release-based commit | Scope |
| --- | --- | --- |
| `88383815` | `368d3c67` | Format inventory |
| `abdd871d` | `036f7a9a` | Protocol-aware SDS implementation |
| `bf224bd0` | `0cbc5fd1` | Firmware tests, dropped-ACK recovery, flash-programming fix |
| `e3d4d397` | `edbc8180` | Booted MM audio and mixed workflow tests |

Cherry-pick conflicts retained the release's existing tests and the new SDS
states in transport wire ownership. Release submodule pins remain unchanged:
DSP `c9a154a3ec3e47b4266bb442d53d8d0cc32239a8`, MCU
`1ae33bff8b223a44bf9462ec6294c3f963206469`. No merge, push, new project checkout,
or unrelated deferred firmware-hook/HI08 cleanup was performed.
The lifecycle repair, processor regression harness, and corrected diagnostics
are committed as `df9f5aa8` on that release-based branch.

Preservation check: the reused MCU submodule is itself a linked worktree.
Submodule setup left a relative `core.worktree` in its shared configuration,
breaking Git status in the older checkout. This local metadata was repaired
by enabling per-worktree configuration and moving the main submodule's worktree
path into its `config.worktree`. Both project worktrees are clean afterward,
retain their distinct MCU commits, and other linked MCU checkout paths resolve
correctly. No source files or submodule pins were changed by this repair.

## MD startup sample loss: corrected setup and causal observation

The previous SDS harness supplied factory-cache metadata to `Hardware` without
supplying the decoded flash. That constructor does not materialize the cache:
this incorrectly combined initialized-cache flags with raw ROM flash. Product
`Device::loadInitialMdFlash` supplies **both**. The harness now does too and
asserts initial flash equals the decoded cache. The old incoherent construction
is available only as an explicit `metadata-only:seconds` diagnostic control.

Correcting the fixture does **not** eliminate early-start sample loss on the
release baseline:

| Corrected release setup | Start | Observed result |
| --- | --- | --- |
| Factory flash, empty patch RAM | MIDI-ready + 0 seconds | Complete, one ACKed sample, PCM subsequently missing |
| Same | MIDI-ready + 1 second | Retry-limit failure, PCM missing |
| Same | MIDI-ready + 5 seconds | Complete PCM retained |
| Restored initialized project with existing sample | MIDI-ready + 0 seconds | Both samples retained |
| Fresh factory initialization followed by reconstructed boot | MIDI-ready + 0 seconds | Complete, ACKed, PCM subsequently missing |

Times are emulated times for these fixtures, not a proposed wait policy.
In particular, the earlier branch's passing fresh-initializer result does
not hold on this release baseline.

A new optional, synchronous NOR-operation observer records decoded flash
commands and MCU time. It does not inspect firmware task lists or use program
counter locations as a readiness gate. The generated 5,201-word sample's
complete program sequence is recognized against an independently decoded PCM
reference, not a predetermined flash address. The corrected early-start trace:

```
NOR SAMPLE PROGRAMMED t=2.829956 offset=4e0018 words=5201
READINESS IMMEDIATE t=2.831383 contents=1
NOR ERASE t=3.179368 offset=4e0000 size=10000
```

An immediate full-PCM search after transport completion succeeds; the same
search after settling fails. The firmware first programs the entire sample
and then issues a sector erase covering it. A passive, no-import control also issues a startup erase sweep,
from `0x2c0000` through `0x4d0000` at approximately 3.081–3.121 seconds, followed
by erases at `0x7a0000`/`0x7b0000`. With the early imported sample, the sweep
extends through `0x4e0000`. A reconstructed, initialized project with a verified
sample performs no observed NOR erases during the corresponding 20-second
no-import control. The incoherent metadata-only fixture also programs then
erases its sample, but at different addresses.

This localizes the loss to an overlapping startup storage-maintenance sequence,
not incomplete UART delivery or an unprogrammed sample. Blank versus initialized
patch/project state matters. It does **not** establish that the timing or erase
policy matches physical hardware. The release still contains the deferred MD
firmware task-list workaround, so peripheral/scheduling causation is not ruled
out. Changing that unrelated workaround would require its own hardware-backed
repair and regression pass; this work does not reinstate or invent one.

## Ownership and lifecycle contract

Keep the existing transport for paced UART delivery, Turbo negotiation, SDS
ACK/retry handling, and safe termination. Add a small device-owned authorization
layer above it; do not duplicate the sender or run firmware on a UI timer.

| Owner | Responsibility |
| --- | --- |
| File preparation | Full-file framing/classification and model-specific admission; owns bytes before start |
| `Device` | Import ticket, readiness prerequisites, state/instance invalidation, cancel/resume authorization |
| `Hardware` / transport | Single MIDI-wire owner, MCU-time pacing, SDS acknowledgements, mode-boundary pauses, cancellation draining |
| Processor | Serializes device calls and advances emulation in host audio callbacks; performs state transactions |
| Editor | File chooser and user confirmation, immutable ticket capture, progress presentation; no direct sender mutation |

`SysexImportTicket` identifies device instance, hardware epoch, state-restore
generation, and request sequence. It is not serialized. Reserving a newer file
selection supersedes a pending one; an active transfer cannot be stolen. The
ticket is captured **before** the asynchronous file chooser and survives into
the receive-mode confirmation callback. Start, cancel, resume, and retirement
reject stale tickets. Resume additionally requires the transport id and exact
receive step. Reopened editors can monitor the current device-owned transfer.

Beginning a real machine-state transaction invalidates pending confirmations
and cancels active delivery even if state preparation subsequently fails.
Successful hardware replacement also changes the epoch. A cancelled file buffer
is owned by the transaction and destroyed after the processor/device lock has
been released. An outer, malformed host container rejected before reaching a
device state transaction is not a selected replacement project.

Start requires a valid local machine, MIDI readiness, no pending project
restoration, and no pending first-run factory-initialization reboot. MM imports
and MD SDS additionally require explicit user confirmation that storage work
has finished and the appropriate receive screen is ready. This confirmation
is an assertion by the user, **not** automatic firmware verification. Raw
`Hardware` APIs remain available for internal/tests; UI imports use `Device`.

The lifecycle exposes `Preparing`, `Transferring`, `AwaitingReceiveMode`,
`DeliveredUnverified`, `Cancelled`, `Failed`, and `Invalidated` (plus idle).
Transport completion maps only to `DeliveredUnverified`; `contentsVerified`
remains false. The UI says “SysEx delivery complete,” explains that bytes
reached MIDI input, and asks the user to check the firmware result. SDS ACKs
are not proof of durable storage. Cancellation stops remaining delivery; it
does not roll back previously imported data or undo an acknowledged sample.

Suspended host callbacks suspend emulated transport time, including reply
timeouts. Progress polling must not advance it. No wall-clock timeout can
truthfully turn a suspended host into a failed firmware operation.

## Processor-level verification

`mdSysexLifecycleTest` constructs the actual JUCE processors with ephemeral
configuration and isolated cache copies under a unique `temp/` directory.
MCP is disabled. ROM search directories are supplied explicitly. Audio runs
through `AudioProcessor::processBlock` with block sizes 64, 127, 256, 511, and
128; the test never calls `Hardware::advance` directly. Startup settling is
fixture preparation, not a shipping readiness algorithm.

The release-based suite passed:

- Correct materialization of the MD factory cache and pre-boot rejection.
- Required SDS/MM receive confirmation, wrong-model rejection, superseded
  file selection, pending cancellation, duplicate confirmation rejection.
- A real SDS import through the processor, suspension after payload bytes are
  already in flight, 10,000 progress polls without
  audio callbacks, resumption, one ACKed sample, and complete independently
  calculated 5,201-word PCM present after settling.
- Actual processor `getStateInformation`/`setStateInformation` round trip;
  hardware-epoch replacement; late confirmation rejected after restore.
- State replacement during partial payload delivery; stale cancellation/resumption/retirement
  rejected; old PCM absent and MIDI ingress idle after restored boot.
- Queued and partial-payload cancellation, released MIDI ingress, and failed
  machine-state preparation invalidation during partial delivery.
- MM startup/confirmation/cancellation and separate-processor ticket isolation.

This is not a DAW/window-automation test: chooser wiring is compiled and
reviewed, while asynchronous-command validity is exercised at the shared
device API. MM authorization uses a minimal framed dump that is cancelled;
it is **not** evidence of MM firmware accepting its contents. The MD complete
PCM check is a test oracle and does not silently become a product guarantee.

Targeted release unit tests `mdTurboMidiUnitTest`, `mdSdsTransferTest`,
`mdFlashTest`, and `mdStateTest` passed. The confidence-runner's three Python
tests passed. The shared JUCE plug-in code and processor test both built.
The real-firmware dropped-ACK fault was rerun with the corrected release cache
setup: injected loss of ACK 5 elicited NAK 6, completed with one retry, retained
the complete 5,201-word PCM, and passed the state-content comparison.

## Outstanding MM release-baseline regression: do not certify shipping

The inherited firmware-workflow tests do not pass on this release-based branch:

- Full public `2 TRI--INV.syx` bank: slot 0's complete 6,132-byte payload is
  found, but slot 1 fails the complete-content check. Its prefix matches
  6,129 bytes; the remaining three expected zero bytes are erased (`ff`).
  A repeat with final flash export reproduces this. Independent analysis of
  all 64 references finds only slot 0's complete waveform; even the first
  1,024 signed-24-bit samples occur intact for only slots 0, 1, 9, 19, 35, 48,
  and 52. This is not merely one trailing-padding discrepancy. Counters show
  449,906 MIDI bytes consumed, zero MIDI overflow, idle ingress, no pending
  panel input, and zero panel overflow. The final full-bank LCD is back at
  the normal performance screen; a visible hang is not required for failure.
- Mixed general → DigiPRO → general: both transport mode boundaries occur,
  and kit 62 reads back correctly, but the complete final kit 63 differs.
  Panel captures show the driver still in the DigiPRO receive screen during
  the attempted next general-receive navigation.

An experiment repeating Exit after the fixture's settling interval did not
fix the mixed result and is not retained as a fix. The shared flash command
fix is identical to the earlier passing branch, so that fix alone is not
sufficient. These observations do not yet distinguish a backend regression,
remaining panel-driver assumptions, and oracle assumptions. The earlier
branch's six passing MM workflows must not be transplanted as release evidence.

The alpha tester's exact waveform file, plug-in build, and reproduction remain
unavailable. Do not claim their “Writing Waveforms” hang is fixed on this
baseline. Next backend work should compare the same public bank across the
release history, record complete flash/panel/UART outcomes, isolate the first
regressing change, and repair the demonstrated hardware/protocol behavior.
Do not merge a deferred private-hook branch or weaken the content oracle to
manufacture a passing result.

## Reproduction and durable artifact index

Build with the repository's existing Release arm64 configuration, testing
enabled. `mdSysexLifecycleTest` additionally requires JUCE; the protocol/flash
and hardware tests can use the JUCE-disabled build.
Without its explicit fixture arguments, the processor test reports skip (77);
with `MD_AUTOMATION_REQUIRE_FIRMWARE=1`, missing arguments fail instead. A
fixture-free CTest invocation is not evidence that the firmware suite ran.

```sh
cmake --build temp/build-sysex-processor --target mdSysexLifecycleTest -j 4
temp/build-sysex-processor/source/elektron/md/mdJucePlugin/mdSysexLifecycleTest \
  /absolute/path/MD-1.63.bin /absolute/path/md-factory.cache \
  /absolute/path/MM-1.32b.bin /absolute/path/mm-patch.bin

MD_SDS_FLASH_TRACE=1 \
temp/build-sysex-release/source/elektron/md/mdLibTest/mdSdsFirmwareTest \
  /path/MD-1.63.bin --generated /path/md-factory.cache boot:0
```

Repeat the latter with `observe:0` (no-import startup control) and
`observe-restored:0` (no new import after reconstructed initialized boot).
The known `boot:0` content loss returns failure; controls returning success
mean the observation finished, not that an import succeeded.

For MM, use `mmSysexWorkflowTest ROM PATCH BANK` and append `mixed` for the
mixed case. `MM_SYSEX_PANEL_PREFIX` captures receive-entry panel stages;
`MM_SYSEX_DIAGNOSTIC` captures the final LCD, flash and ingress counters.
The decoder observer runs synchronously under the flash lock: diagnostic
callbacks must never reenter the MCU or mutate its state.

Uncommitted diagnostic artifacts in the reused worktree's `temp/`:
`sysex-lifecycle-run.log`, `sysex-lifecycle-run-final.log`, `sysex-lifecycle-build.log`,
`sysex-release-regressions.log`, `sysex-runner-tests.log`,
`release-readiness-*.log`, `release-trace-boot:0.log`,
`release-trace-observe0.log`, `release-trace-observe-restored0.log`,
`release-md-immediate-trace.log`, `release-sds-drop-ack.log`,
`release-mm-mixed*.log`, `release-mm-bank*.log`, and corresponding panel
captures. Fixture provenance/hashes remain in the original
[format inventory](md_mm_sysex_format_coverage.md) and corpus manifest.

Remaining limits: physical-hardware storage readiness/commit signal;
release MM firmware workflow failures; actual DAW callback/UI behavior;
other operating systems, firmware versions, and the tester's exact file.
The focused ownership repair is useful independently, but the end-to-end
SysEx feature is not yet release-certified.
