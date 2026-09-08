# Machinedrum / Monomachine SysEx files

To send a user-data SysEx file to the emulated Machinedrum or Monomachine,
right-click anywhere on the instrument UI and choose **Send SysEx File...**.
The context-menu item shows transfer progress when the menu is reopened and
becomes **Cancel SysEx Transfer...** while a transfer is active. Cancellation
terminates any partial SysEx message before releasing the emulated MIDI wire.

The sender accepts one or more complete Elektron user-data dump messages for the
active machine, up to 8 MiB: globals, kits, patterns, songs, and Monomachine
DigiPRO waveforms. Machinedrum UW SDS samples, concatenated sample banks, and
sample-name messages are also supported. Framing, 7-bit data, model identity, dump-command identity,
checksum, and declared length are validated for every message. Files for the
other machine, MIDI control/request commands, malformed/corrupt streams, and OS
updates are rejected.

Monomachine firmware accepts data dumps only while the matching receive screen
says **WAITING**. After parsing the selected file, the plug-in asks for the
appropriate receive screen. Use **GLOBAL > FILE > SYSEX RECV** for kits, patterns, songs,
globals, and backups. DigiPRO waveform files instead use **GLOBAL > FILE >
DIGIPRO MGR > RECEIVE**. After a DigiPRO transfer reaches its last waveform,
press **EXIT/NO** and wait for **WRITING WAVEFORMS** to finish. The Machinedrum
does not require a receive screen for ordinary dumps. For SDS samples, confirm
that booting and any earlier **CLEANING/LOADING** have finished before sending.
Sample import can overwrite existing ROM slots and stops the sequencer.

Mixed files preserve message order. A Monomachine file changing between general
dumps and DigiPRO pauses for the corresponding receive-screen change. A
Machinedrum file with ordinary data following samples pauses until
**CLEANING/LOADING** has finished. Close the notice, prepare the machine, then
right-click and choose **Resume SysEx Transfer - machine is ready**. Cancellation
remains available. Data already imported is not rolled back.

SDS uses closed-loop acknowledgements with bounded retries and device WAIT/
CANCEL handling. Archived SDS device IDs are retargeted to the MD's fixed device
ID 0, with the packet checksums adjusted. The sample position remains unchanged;
the firmware's selected receive destination still governs placement. SDS on
Monomachine, handshake-only files, Sample Dump Extensions, and OS updates are
identified but not imported. There is no automatic open-loop fallback that can
turn an unresponsive receiver into a successful transfer.

Imported Monomachine DigiPRO data is included in newly saved plug-in/project
state. This adds a fixed 2 MiB user-flash image, with an independent CRC, to the
existing 1 MiB patch-RAM snapshot. Older version-1 Monomachine states remain
readable; because they predate user-flash persistence, they restore patch RAM
only and retain the user flash initialized by the loaded ROM.

The sender negotiates TurboMIDI with the emulated firmware and falls back to
standard MIDI speed when negotiation is unavailable. Transfer completion means
the bytes have drained through the emulated MIDI UART; the machine's display is
authoritative for whether its firmware accepted and imported the data. SDS
completion additionally requires acknowledgement of the last sample packet.
It does not mean the firmware has finished cleaning flash or loading DSP sample
memory. Wait for those operations before playback or another import.
While it owns that serial wire, a MIDI event already in flight is allowed to
finish; subsequent host notes/controllers and automation-generated MIDI wait
behind the file. MIDI clock already queued at the ownership boundary crosses
first, and later clock traffic waits with the other input rather than being
inserted into the SysEx stream.

The host must keep processing the instrument while a transfer is active. If the
emulated MIDI port does not advance for five seconds, the UI warns that the
transfer is paused. Resume audio processing and disable plug-in bypass or host
suspension; the right-click cancellation command remains available.

The sender retains at most one file buffer, bounded by the 8 MiB input limit,
and a bounded vector of message descriptors prepared outside the device lock.
Completion leaves that storage available for control-plane retirement rather
than freeing it on the real-time scheduler. The editor normally retires it on
the next progress tick; reopening an editor retires a transfer that completed
while the editor was closed, and starting another transfer also retires an
unobserved terminal buffer outside the plug-in's process/device lock.
Cancellation hands the buffer directly to its control-plane caller for the same
off-lock destruction.
Descriptors remain owned by the transport until its next start or destruction;
starting a new transfer swaps the old descriptor allocation back to the caller
for off-lock destruction as well.

This operation is intentionally not part of the Escape/settings overlay.
Settings are persistent configuration, while sending a file is an active
machine operation with progress and a firmware-owned result.

## Verification

Build and run the deterministic transport and project-state regressions with:

```sh
cmake --build <build-directory> --target mdTurboMidiUnitTest mdSdsTransferTest mdStateTest
ctest --test-dir <build-directory> --output-on-failure \
  --tests-regex '^(mdTurboMidiUnitTest|mdSdsTransferTest|mdStateTest)$'
```

The transport suite covers negotiation fallback, the documented TurboMIDI
handshake, checksum and message validation, concurrent transfer starts, MIDI
wire arbitration, suspended processing followed by resume, cancellation,
terminal payload retirement, and concurrent progress readers.
The SDS suite adds sample-length/sequence validation, multiple resolutions,
packet-number wrap, device-ID retargeting, ACK/NAK/WAIT/CANCEL, lost/wrong replies,
retry exhaustion, queue overflow, UART-drain timing, mixed MD data, and MM
receive-screen pause/resume including stale-action rejection.

For firmware-level verification with legally obtained firmware and user-data
files, build `mdUserSysexFirmwareTest` and run one of:

```sh
mdUserSysexFirmwareTest md <firmware.bin> <user-data.syx> [first|cancel]
mdUserSysexFirmwareTest mm <firmware.bin> <1MiB-patch-ram.bin> \
  <user-data.syx> [first|cancel]
```

The harness drives the Monomachine receive screens, interleaves clock,
controller/automation-like traffic, and ordinary MIDI with the transfer, waits
for the emulated UART to drain, and requires persistent RAM or flash mutation.
For DigiPRO it also serializes project state and verifies byte-exact user-flash
restoration in a fresh hardware instance. `first` limits a concatenated file to
its first message; `cancel` verifies partial-message termination and return to
an idle transport.

### Verification record

On 2026-09-04, a clean macOS arm64 Release build from the feature commit passed
`mdTurboMidiUnitTest` and `mdStateTest`. Machinedrum and Monomachine Standalone,
VST3, and AU targets built successfully; all six bundles passed strict ad-hoc
signature verification, both AU property lists were valid, and the project
plug-in tester loaded both VST3s and passed its audio-bus and automation/state
smoke checks.

The same build completed firmware-backed imports of a 777,212-byte Machinedrum
backup, a 288,472-byte Monomachine backup, and a 449,728-byte Monomachine
DigiPRO bank. They respectively changed 178,359 patch-RAM plus 1,025,724 flash
bytes, 148,267 patch-RAM bytes, and 378,314 user-flash bytes. The DigiPRO run
also passed project-state serialization and byte-exact restoration into fresh
hardware. A separate Machinedrum run cancelled after 40 of 777,212 bytes and
verified a terminal transport with idle MIDI ingress.

A 2026-09-08 public-corpus investigation found that valid Machinedrum UW SDS
sample files were rejected by the baseline product-envelope validator before
they reach firmware. It also records the complete documented MD/MM command
inventory, SDS message grammar, protocol/transport implications and
reproducible input hashes. See
[MD/MM SysEx format coverage and rejected-sample investigation](md_mm_sysex_format_coverage.md).
The subsequent implementation, firmware evidence, reproducible test commands,
and remaining boundaries are recorded in
[SDS sender implementation and acceptance](md_mm_sysex_sds_implementation.md).

This local record does not replace the normal Windows x64, Linux, or macOS
universal CI jobs. Those remain required before merge because they exercise
other compilers, sanitizers, CPU architectures, and release packaging.

## Known limitations

- Transfers advance only while the host processes the instrument. Some hosts
  suspend processing for bypassed, muted, or inaudible plug-ins.
- UART-drained completion cannot prove firmware acceptance. Follow the
  instrument display, especially the Monomachine's receive and flash-writing
  prompts.
- Import is deliberately limited to complete, checksum-valid user-data dump
  messages recognized for the active model, with an 8 MiB file-size limit.
- SDS support is for the MD UW receiver and ordinary Sample Dump Standard,
  not every universal MIDI message or Sample Dump Extension. Unknown/control
  commands and firmware updates are not passed through as user data.
- If a transfer completes with no editor open and no later transfer starts, its
  bounded file buffer remains owned by the machine until an editor opens or the
  plug-in instance is destroyed. This avoids an unbounded-time deallocation on
  the real-time scheduler.
- Platform and plug-in-format packaging remains the responsibility of the
  normal project CI matrix; the firmware harness requires user-supplied ROMs
  and therefore is not suitable for public CI artifacts.
