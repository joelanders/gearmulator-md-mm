# Send SysEx: firmware-backed confidence testing

Historical baseline: these results belong to the deferred firmware-hook
integration branch. See the [release-baseline lifecycle investigation](md_mm_sysex_lifecycle.md)
for the corrected cache fixture, processor-level tests, and outstanding MM
release-baseline regressions. The flash-programming fix below is necessary but
does not by itself certify DigiPRO imports on the newer release baseline.

2026-09-08. Follow-up to [the initial implementation](md_mm_sysex_sds_implementation.md),
using the existing `gearmulator-md-mm-sysex-sender-v2` worktree. This batch targets
the complete downloaded corpus, faults against real emulated firmware, and
readiness. It is not cross-platform/plugin-host acceptance or a claim that every
historical/custom firmware format works on stock firmware.

The [second batch](md_mm_sysex_workflow_testing.md) adds fully booted MM
playback/persistence, mixed receive-screen workflows, cancellation boundaries,
and deeper MD readiness counterexamples.

## Findings that changed the code

### Lost SDS acknowledgements

Dropping the ACK for data packet 5 caused the production sender to resend 5.
MD OS 1.63 then sent NAK **6**, requesting the next packet it expected. Previously
we ignored that response because its number differed from the current packet;
another retry eventually elicited CANCEL. The unit-test receiver had simply
ACKed duplicates and did not reveal this behavior.

`TurboMidiTransfer` now honors a next-packet NAK only after a retransmission,
only from the expected device, and only between data packets within the same
sample. It cannot acknowledge a header or the final sample packet this way.
Other packet mismatches remain ignored. Deterministic unit tests cover both
the ordinary case and packet-number wrap (127 to 0).

The real-firmware dropped-ACK case subsequently completed with one retry and
the entire expected PCM sequence present. The delayed-ACK case also recovered.

### Truncated MM DigiPRO waves despite transport completion

The independent waveform oracle found incomplete imports in `2 TRI--INV.syx`
and `5 FM---INV.syx`. For TRI, slot 1 matched the first 2,722 decoded bytes, then
became erased flash. This was not a SysEx checksum or file-framing problem.

The shared `hwLib::Am29f` decoder considered longer erase-command sequences
before executing an already armed Program operation. A programmed word whose
low byte was `AA`, at an unlock-shaped address, could consequently be consumed
as another command cycle. Firmware stalled waiting for that unwritten word.

The next write after a completed Program command is now unconditionally its
payload, before further command matching. The AMD command table describes the
fourth program cycle as the target address/data, rather than another unlock
cycle: [manufacturer datasheet](https://www.infineon.com/assets/row/public/documents/10/57/infineon-am29f002b-am29f002nb-2-megabit-256-k-x-8-bit-datasheet-additionaltechnicalinformation-en.pdf).

`mdFlashTest` now exercises all 65,536 payload words with each command-address
wiring configuration, including the MM unlock-address collision, and checks
sector erase afterward. The shared decoder is also used outside MD/MM; this
small fix therefore has a wider integration surface than the SysEx sender.

## Corpus oracle and compatibility

The inputs and original download references/hashes remain in
[the format investigation](md_mm_sysex_format_coverage.md). No third-party bank,
ROM, cache, or downloaded manual is committed.

The stronger `mdUserSysexFirmwareTest` now verifies:

- MD: request every final `(command, slot)` back from firmware and compare every
  supplied payload byte. If a file writes a slot repeatedly, its last message
  defines the expected result. Repeat readback on a fresh machine reconstructed
  from encoded/decoded project state, after boot settling.
- Legacy MD patterns: stock 1.63 expands 2,763-byte 32-step messages into
  5,410-byte 64-step messages. Compare all supplied payload bytes; do not claim
  the extra, unsupplied tail has been independently verified.
- MM: independently unpack each DigiPRO waveform's 7-bit encoding and find its
  complete 6,132-byte sequence in user flash. Repeat after state reconstruction.
  This proves contents and persistence, not every slot's playback or placement;
  identical waveforms can match the same flash sequence.
- SDS: independently decode every declared sample word, verify complete PCM
  contents in flash, encode/decode state, and verify contents on a newly booted
  machine. The previous generated-wave playback oracle remains available.

Three structurally valid MD files contain kits that stock OS 1.63 does **not**
support: `md-d1516-track.syx`, `md-hemmelig-liveset-202111.syx`, and
`md-hemmelig-liveset-202204.syx`. Their mismatching kits carry version 64.1
(`40 01`), rather than stock kit version 4.1. The X.04 author's release notes
explicitly identify this format change and the older-firmware incompatibility:
[original release archive](https://github.com/jmamma/MIDICtrl20_MegaCommand/releases/download/3.10/Machinedrum_SPS1-UW_OS_X.04.zip).

Those files must not be described as successful complete imports on stock 1.63.
The corresponding patterns can import while the kits do not. Rewriting the
version byte would not supply the new machines, tonal tuning, or other custom
firmware semantics. The parser intentionally still recognizes these files;
recognition does not imply receiver compatibility. No matching custom 8 MiB
ROM was available for a positive custom-firmware run.

Final corpus results, with stock MD 1.63 and MM 1.32b on macOS arm64:

| Group | Files | Firmware/content result |
| --- | ---: | --- |
| MD stock-format dumps/banks | 5 | All supplied payloads match firmware readback, before and after state reconstruction |
| MM DigiPRO banks | 24 | All 64 complete waveforms per bank match flash, before and after state reconstruction |
| MD UW SDS files | 2 | Complete decoded PCM survives import and a newly booted state reconstruction |
| MD archives containing X.04 kits | 3 | Not complete imports on stock 1.63; 17 incompatible kits in total fail readback |

That is **31 positive end-to-end corpus results and 3 demonstrated firmware
incompatibilities**, not 34 successful imports. The 24 MM banks cover 1,536
waveforms. The five positive MD files cover 480 final command/slot pairs across
their independent instances. The other-product file remains outside this
in-scope corpus and rejected by validation.

## Fault injection

`sdsFaultWire.h` is test-only. It connects the production `TurboMidiTransfer`
to the actual emulated UART and taps firmware replies, advancing by measured
MCU cycles. It uses existing interfaces, with no shipping fault switches or ROM
patches. This is not a synthetic receiver that blindly ACKs each packet.
The isolated sender bypasses Hardware's ingress arbitration; normal corpus
imports exercise that integration separately.

Modes include unmodified control, corrupted data packet, dropped ACK, delayed
ACK (three emulated seconds), duplicate ACK, explicit WAIT followed by the
delayed real ACK, persistent response loss, dropped header ACK, and lost final
acknowledgement. WAIT is deliberately injected; the underlying packet ACK is
still firmware-generated. Successful fault cases require complete decoded PCM
and a byte-exact flash state-codec round trip. Terminal failures must release
the wire, drain the UART, and not claim an acknowledged sample. They do not
promise rollback of data already received.

The unmodified, corruption, dropped data-ACK, delayed-ACK, duplicate-ACK, and
WAIT cases completed with correct sample contents. Corruption, dropped data-ACK,
and delayed-ACK required one retransmission each. Delayed replies remain
scheduled after transfer completion and their eventual delivery is asserted.

Persistent response loss and loss of the final ACK exhaust the bounded retry
budget rather than report success. In the latter case PCM is already stored,
but the sender correctly reports zero acknowledged samples. Losing the header's
ACK after firmware has sent WAIT reaches the bounded WAIT timeout instead;
silence cannot distinguish an unfinished WAIT from a lost reply. These are
explicit safe failures, not successful imports. Fresh-import recovery is also
required after each of these three failure cases.

## Readiness: an unresolved firmware boundary

The generated matrix uses fresh MD 1.63 instances and an initialized UW factory
cache. Initial imports begin 0, 1, 5, or 20 emulated seconds after
`isFirmwareMidiReady()`. Separate cases start a distinct second sample 0, 1, 5,
or 20 seconds after completing or cancelling the first. Completed first samples
must remain present in the back-to-back cases.

Starting at 0 or 1 seconds reproduced **ACKed but missing sample contents**, even
after allowing twenty more seconds before inspection. The 5- and 20-second
boot cases passed. These are observations for this ROM/cache/fixture, not a
universal minimum-safe delay.

All eight back-to-back and cancellation/restart cases passed at the tested
delays, including preservation of the first completed sample. One cancellation
case (one-second delay) needed a retry and still retained the complete new
sample. Overall the twelve-case matrix has ten retained-content results and
two reproducible early-boot losses.

An additional `probe:0` experiment requested a complete kit from firmware before
starting SDS. Firmware answered in roughly 10 ms, yet the subsequent sample was
still lost. Thus even a successful ordinary SysEx request is not a reliable
sample-storage-ready signal. Neither a MIDI-ready flag nor a successful ACK or
readback should be promoted to that guarantee.

Production still requires the user to wait for boot/CLEANING/LOADING and confirm
readiness. This batch does not replace that requirement with an arbitrary timer,
screen-pattern heuristic, or a handshake demonstrated to be insufficient.
The early-boot probes intentionally remain nonzero results: documenting a
known hazard is not the same as fixing it.

## Reproduction

Build the test targets in the existing configured build:

```sh
cmake --build temp/build-sysex-investigation --target \
  mdUserSysexFirmwareTest mdSdsFirmwareTest mdSdsTransferTest mdFlashTest
```

The repository-owned runner captures per-case logs, commands, input/fixture
SHA-256 hashes, exit statuses, and durations in a new output directory:

```sh
python3 source/elektron/md/mdLibTest/runSysexConfidence.py \
  --suite all \
  --bin-dir temp/build-sysex-investigation/source/elektron/md/mdLibTest \
  --md-rom /path/to/MD-1.63.bin --md-cache /path/to/factory.cache \
  --mm-rom /path/to/MM-1.32b.bin --mm-patch /path/to/mm-patch-ram.bin \
  --corpus temp/sysex-web-corpus-20260908 \
  --output temp/confidence-new-run
```

Use `--suite corpus`, `fault`, or `readiness` to run a subset. MM/corpus arguments
are unnecessary for the latter two. It returns nonzero if any probe fails,
including known incompatibilities and early-boot hazards; it does not silently
relabel these as successful imports. Each firmware executable can also be run
directly. `mdSdsFirmwareTest ... --generated factory.cache probe:0` reproduces
the unsuccessful kit-readback readiness experiment.

Local evidence remains ignored under `temp/`: `confidence-final-mm-*.log`,
`confidence-persist-*.log`, `confidence-final-01_*.log`,
`confidence-final-md-{d1516,hemmelig}*.log`, `confidence-final-fault/`,
`confidence-recovery-*.log`, `confidence-late-*.log`, and
`confidence-final-readiness/report.json`. The eight targeted regression tests
and SDS AddressSanitizer/UndefinedBehaviorSanitizer suite pass. MD and MM JUCE
shared-code targets also rebuild successfully; these are not plugin-host smoke
tests or a whole-repository/cross-platform CI result.
