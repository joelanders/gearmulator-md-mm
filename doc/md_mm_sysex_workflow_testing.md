# Send SysEx: booted playback and workflow tests

2026-09-08. Second confidence batch, following
[the firmware-backed corpus and fault tests](md_mm_sysex_confidence_testing.md).
Same reused `gearmulator-md-mm-sysex-sender-v2` worktree, stock MD 1.63 and
MM 1.32b, macOS arm64. No ROMs, banks, caches, manuals, or generated recordings
are committed. Sources and corpus hashes remain in
[the format investigation](md_mm_sysex_format_coverage.md).

## What this adds

`mmSysexWorkflowTest` boots the real MCU/DSP firmware, imports through the
production sender and UART, commits DigiPRO reception using panel events,
encodes/decodes project state, destroys the original machine, constructs a new
one, and **boots that new instance** before checking contents and playback.
This closes the previous MM test's constructor-only persistence limitation.

Full-bank cases independently unpack and find all 64 complete 6,132-byte
waveforms in flash, both before and after reboot. Audio checks select slots
0, 1, 45, and 63 on DPRO-DDRW through documented machine-assignment SysEx and
MIDI CC messages. These include the slots implicated in the earlier flash
programming bug. The renderer checks both channels for non-finite samples and
compares the left channel with the independently decoded waveform.

The empirical reference is the first 1,024 signed, big-endian 24-bit words in
the unpacked waveform. At MIDI note 48, linear interpolation at the expected
pitch gives strong agreement with firmware audio. Correlation permits an
unknown oscillator phase and gain/DC changes; it must exceed 0.90, with a
separate non-silence requirement. The remaining waveform bytes are checked for
persistence, not used to claim a complete model of the oscillator's other
resolution levels. This is not bit-exact hardware audio equivalence.

The ROM-free `mmDigiProImportAudioOracleTest` has a positive analytic waveform
control plus negative controls for silence, DC, noise, wrong pitch, another
periodic waveform, missing/truncated reference data, and NaN/Inf—including an
isolated invalid sample outside the correlation window. It compiles without
fast-math so those checks remain meaningful.

An initial diagnostic mistakenly assumed the MIDI value was the waveform slot
or twice its index. On the tested firmware, value 2 still selects D01. The
selected odd values `2 * slot + 1` correctly reach D01, D02, D46, and D64, as
checked against panel output and distinct waveform/audio references. The
incorrect selection was a test-setup defect, not evidence of an import defect.

## Mixed imports and cancellation

The mixed fixture is generated from an actual firmware kit dump using the
documented MM run-length and 7-bit encodings. It contains:

1. A distinctly named kit at zero-based slot 62.
2. DigiPRO waveforms at slots 0, 1, 45, and 63.
3. A differently named kit at slot 63.

The harness navigates the actual general-SysEx and DigiPRO receive screens. It
resets remembered menu cursors rather than assuming every entry starts at the
same item. At both sender pauses it checks that payload progress stays fixed,
rejects wrong transfer IDs and receive steps, changes the receive screen, and
requires the valid resume to work exactly once.

Additional modes cancel at each of the two boundaries, require MIDI to drain
and become available, reject the cancelled resume, and start a fresh import.
An old resume is also delivered while the **new** transfer is paused, not just
while it is running. The number of boundaries is asserted (2 normally, 3 or 4
with the respective cancellation/restart), so a skipped stage cannot pass.

Both complete decoded kit payloads are requested back from firmware; waveform
contents and all four selected sounds are checked. These checks repeat after
booting the reconstructed project. Cancellation does not promise rollback of
already received data. These tests cover the Hardware API and firmware panel
events, not JUCE dialog callbacks inside a DAW.

## MD sample-storage readiness

`mdSdsFirmwareTest` adds restored-project, no-local-cache, and fresh-initializer
cases. Restoration first imports and verifies a sample, round-trips project
state, reconstructs its flash, and cold-boots a new instance. It then sends a
distinct second sample and verifies that both survive. The initializer case
starts without cached flash, advances the normal scheduler to the existing
initialization/reboot boundary, and reconstructs from its initialized flash
and patch RAM before testing SDS.

| Setup / candidate | When the tested import starts | Content result |
| --- | --- | --- |
| Empty patch RAM + initialized factory cache | At MIDI readiness | ACKed, but sample later missing |
| Same cached setup | Five emulated seconds later | Retained |
| Reconstructed project with an existing sample, with local cache | At MIDI readiness / five seconds later | Both samples retained |
| Same reconstructed project, no local cache supplied | At MIDI readiness / five seconds later | Both samples retained |
| Fresh initialization followed by reconstructed boot | At MIDI readiness / five seconds later | Retained |
| Second import immediately after a completed import | No extra delay | Both samples retained |
| Fresh import immediately after cancellation | No extra delay | New sample retained |
| Cached setup, wait for two seconds of flash-write inactivity | MCU time about 17.576 s | Retained |
| Cached setup, wait for sampled PC `0x201124` | MCU time about 5.060 s | ACKed, but sample later missing |

That is twelve probes: ten retained-content results and two reproduced
early-start losses. Nonzero exits for those two losses remain failures; the
runner does not relabel them as successful imports. Contents are inspected
after allowing twenty more emulated seconds to settle.

Optional `ReadinessTrace` observations record MCU time, MIDI readiness, cache
and reboot flags, time since the last flash write, LCD hashes/images, and
sampled-PC histograms. They do not write firmware RAM, patch ROMs, replace UART
callbacks, or alter the normal scheduler. The cached setup already reports
cache/reboot availability before MIDI readiness; these flags are not sample
storage readiness. The sampled PC occurs during both startup and steady state
and demonstrably does not qualify an import. Panel captures include the
firmware's PREPARING FLASH screen.

The successful flash-quiet probe is **not** a certified gate: it is a timing
heuristic tested on one startup fixture, and read-only sample loading need not
produce flash writes. `flashIdleCycles()` is zero before a dirty flash write,
not a general measure of all storage activity. The previous kit-readback
counterexample also still applies. No automatic readiness guarantee or new
shipping delay is introduced. Users must still wait for boot and storage work
to finish and explicitly confirm readiness.

## Reproduction

Build `mmSysexWorkflowTest` and `mdSdsFirmwareTest` with the existing headless
build. The original runner now has `workflow`, `storage-readiness`, and `next`
suites; `next` combines this batch. It records executable and fixture SHA-256
hashes as well as per-case logs, commands, exit statuses, and durations:

```sh
python3 source/elektron/md/mdLibTest/runSysexConfidence.py \
  --suite next \
  --bin-dir temp/build-sysex-investigation/source/elektron/md/mdLibTest \
  --md-rom /path/to/MD-1.63.bin --md-cache /path/to/factory.cache \
  --mm-rom /path/to/MM-1.32b.bin --mm-patch /path/to/mm-patch-ram.bin \
  --corpus temp/sysex-web-corpus-20260908 \
  --output temp/workflow-confidence-new-run
```

`workflow` needs only the MM/corpus arguments. `storage-readiness` needs only
the MD arguments and intentionally returns nonzero for the known hazards.
Set `MD_SDS_READINESS_TRACE` to an ignored output prefix for LCD/PC traces;
`MM_SYSEX_PANEL_PREFIX` records receive-menu navigation, and
`MM_AUDIO_DIAGNOSTIC` records panel images, raw little-endian float audio on
this platform, and cross-wave correlations. Diagnostic paths are optional and
are not needed for acceptance.

Runner self-tests (mocked subprocesses, no firmware required):

```sh
python3 -B source/elektron/md/mdLibTest/runSysexConfidenceTest.py
ctest --test-dir temp/build-sysex-investigation \
  -R mmDigiProImportAudioOracleTest --output-on-failure
```

## Completed results

All six firmware workflows exited successfully:

| MM case | Contents after import and booted restore | Audio comparisons |
| --- | --- | ---: |
| `1 SINE-EXT.syx` | All 64 complete waveform payloads | 8 passed |
| `2 TRI--INV.syx` | All 64 complete waveform payloads | 8 passed |
| `5 FM---INV.syx` | All 64 complete waveform payloads | 8 passed |
| Mixed TRI import | Both complete kits and four waveforms | 8 passed |
| Cancel before DigiPRO, then restart | Both complete kits and four waveforms | 8 passed |
| Cancel before final kit, then restart | Both complete kits and four waveforms | 8 passed |

Across the 48 comparisons, correlation ranged from **0.9242772 to 0.9869702**.
The lowest result was FM slot 45 before reboot; it reached 0.9318589 after
reboot. The 0.90 threshold was established before running this FM fixture, not
lowered to accommodate it. Flash searches verify complete contents but are
not an independent placement oracle for unplayed or identical waveforms.

The nine targeted CTest suites passed: MD library, UART registers, Turbo MIDI,
SDS, automation MIDI, flash, state, audio queue, and the new DigiPRO import
audio oracle. The three Python runner tests passed. MD and MM JUCE shared-code
targets rebuilt successfully; this was not a DAW-host test or a complete
cross-platform CI run. No additional shipping-code change was needed in this
batch; changes are test harnesses, runner coverage, and durable notes.

Local evidence is ignored under `temp/`: `mm-workflow-final-bank-{1,2,3}.log`
(SINE, TRI, FM respectively), `mm-workflow-final-mixed.log`,
`mm-workflow-final-cancel-before-{digipro,general}.log`,
`md-readiness-*.log` and associated panel captures,
`next-batch-regressions.log`, and `next-batch-ui-build.log`.

## Remaining limits

This batch does not supply a universally trustworthy MD sample-ready signal,
test every DigiPRO slot's playback, model every pitch/machine/effect, or replace
plugin-host/cross-platform acceptance. The alpha tester's exact failing bank
and build have not been supplied; matching the earlier Writing Waveforms bug
does not establish that every such report has the same cause. Custom X.04 kit
support still requires matching firmware, not header rewriting.
