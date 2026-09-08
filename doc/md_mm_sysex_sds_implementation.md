# Protocol-aware MD/MM SysEx import

Implemented 2026-09-08 in the reused `gearmulator-md-mm-sysex-sender-v2`
worktree, following the [public-file investigation](md_mm_sysex_format_coverage.md).
This is user-data import, not an OS updater or an unrestricted MIDI command
player. The alpha tester's own rejected file is still needed to establish that
their failure is the same SDS rejection reproduced by the public corpus.

The [subsequent firmware-backed confidence batch](md_mm_sysex_confidence_testing.md)
adds full-content corpus checks and real-firmware fault/readiness probes. It
exposed and fixed lost-ACK recovery and shared MM flash-programming defects,
and confirms that recognized X.04 kits are incompatible with stock MD 1.63.

## Supported input and boundaries

`mdsysexfile.h` parses the complete file before device interaction. Preparation
is now the single validation gate, including for non-UI callers. The prepared
object carries model identity, owned file bytes, and ordered message
descriptors. `Hardware` refuses a prepared transfer for another model.

Accepted families:

- MD/MM global (`50`), kit (`52`), pattern (`67`), and song (`69`) dumps;
- MM DigiPRO (`5d`) dumps and concatenated waveform banks;
- MD UW ordinary SDS headers and packets (`f0 7e dd 01/02 ... f7`), including
  multiple complete samples and final partially used packets;
- MD four-character sample names (`73`), alone or between an SDS header and
  its first packet;
- ordered mixtures of complete user-data sections, with readiness pauses at
  the protocol boundaries described below.

Elektron checksum/footer checks remain in place. Version/revision bytes are
not hard-whitelisted: the downloaded MD kit version byte `40`, for example,
continues to validate. Canonical product padding is zero, but the old sender's
tolerance of other 7-bit values in that unused/base-channel field is preserved.
Recognition should not introduce unrelated restrictions on previously admitted
dump headers.

SDS validation checks fixed header/packet lengths, 7-bit data, resolution
8–28 bits, nonzero period and sample length, loop type/ranges, declared word
count, consistent device ID within each sample, packet numbering modulo 128,
and packet XOR. Packet count follows the number of whole sample words that fit
in a 120-byte payload, including resolutions requiring two, three, or four
MIDI bytes per word. Unused last-packet padding is not decoded as more samples.
Other messages cannot interrupt an incomplete sample, except its optional name.

The MD receiver is fixed at SDS device zero. Preparation first validates the
archived stream, then changes its SDS device bytes to zero and updates each
packet XOR accordingly. It does not change the sample-position field. Actual
placement still follows the firmware's general/specific receive workflow.

Controls, requests, unknown Elektron commands, firmware-update records,
TurboMIDI messages, SDS handshake files, Sample Dump Extensions, other
manufacturers/products, and other universal messages receive explicit rejection
categories. In particular, the `7e` byte in universal SDS is not confused with
Elektron product command `7e` (OS update). Generic universal EOF is not needed
to complete an ordinary SDS sample; the header's word count determines its end.

## Transport and real-time ownership

`TurboMidiTransfer` retains its existing MIDI-wire arbitration and negotiated
speed handling. Ordinary dump runs stream as before; SDS introduces message
boundaries and receive handling:

1. Send the header, wait for its ACK, then send an optional MD name.
2. Send one data packet and drain the emulated UART before starting its reply
   deadline. Preserve replies that arrived during draining.
3. ACK advances the plan; NAK or a missing reply retries the current message.
   There are at most three retransmissions per message. Progress never counts
   retransmitted bytes twice.
4. Explicit WAIT holds transmission. Reply timeout is two seconds; an explicit
   WAIT has a bounded thirty-second deadline. These use emulated time, not wall
   time, and active sensing continues while waiting on an established Turbo link.
5. Receiver CANCEL, exhausted retries, excessive WAIT, or response-queue
   overflow stops the transfer. Cancellation closes any partial message and
   sends SDS CANCEL before releasing the wire. It does not roll back imports.

There is deliberately **no automatic open-loop fallback**. The emulated MD
provides the return connection; silence must not become a claimed successful
sample import. Turbo negotiation can still fall back to ordinary MIDI speed,
with SDS acknowledgements still required.

The final TurboMIDI reply is observed before MD 1.63 has necessarily finished
its speed-change/UART-reset routine. Sending immediately lost the initial SDS
header in the firmware probe, causing one header retry. A ten-millisecond
emulated settling interval after speed-result 2 removed that loss; the
subsequent firmware runs completed without retransmissions. This interval is an
emulator/firmware compatibility observation, not a new MIDI standard requirement.

All file parsing, descriptor allocation, and device-ID retargeting occur outside
the process/device lock. Start swaps both allocations; service uses fixed
queues and does not allocate. Cancel and terminal byte-buffer retirement keep
destruction on the control plane. Descriptor storage stays bounded and is
returned on the next start or freed with the hardware. Progress adds coherent
error/retry/sample-ACK counts and a service heartbeat, so device WAIT or a
user-readiness pause is distinguishable from a suspended host.

## Receive screens and sample loading

The file is classified before prompting. MM gets the receive-screen instruction
for its first section. MD sample files get an overwrite/sequencer-stop warning
and a readiness confirmation: finish booting or any previous CLEANING/LOADING
before starting. The existing MIDI-ready flag describes the MIDI interface,
not completion of the firmware's sample-storage work.

A mixed MM file pauses when changing between general receive and DigiPRO.
The notice can be dismissed so the user can navigate the panel. A separate
**Resume SysEx Transfer - machine is ready** menu action continues the plan;
cancel remains available. Resume checks the transfer identity and message step,
so an old action cannot resume a replacement transfer or another section.

For MD, consecutive SDS samples continue as a bank. Ordinary messages following
a sample bank pause until the user confirms CLEANING/LOADING has finished. The
Turbo link is allowed to reset during that pause; resumption also observes its
reset interval. Sending those following messages immediately is unsafe: the
final sample ACK does not prove the DSP loading stage is complete.

At end of file, the UI reports acknowledged samples but still tells the user to
wait for the display's completion before playback or another import. The
firmware harness uses twenty seconds after initial MIDI readiness and generous
post-import settling before its audio assertions. These are fixture waits,
not claims that the interface exposes a sample-ready acknowledgement. An early
fixture import or playback attempt produced missing sample contents or silence;
the settled runs passed. This is why the production readiness prompts matter.

## Verification

The ignored public corpus remains at `temp/sysex-web-corpus-20260908`; its URLs,
hashes, and licensing/redistribution boundary are in the investigation note.
No third-party bank, downloaded manual, ROM, or factory cache is committed.

On macOS arm64 Release:

- All 34 in-scope public files validate: eight MD ordinary dumps, 24 MM DigiPRO
  banks, and the two previously rejected MD SDS files. The 35th file, from a
  different Elektron product, remains rejected.
- `mdSdsTransferTest`, `mdTurboMidiUnitTest`, `mdStateTest`, `mdFlashTest`,
  `mdLibTests`, `mdAutomationMidiTest`, `mdAudioQueueTest`, and
  `mdUartRegisterTest` pass.
- The SDS parser/transport suite also passes AddressSanitizer and
  UndefinedBehaviorSanitizer, including 2,000 deterministic mutated inputs.
- MD and MM JUCE shared-code targets build, compiling the changed editor and
  context menu for both products.
- Real MD OS 1.63 accepts `01_DUB1_BD01.syx` (65,693 bytes, 20,656 sample words)
  and `01_RNB1_BD01.syx` (24,291 bytes, 7,601 sample words), with zero retries.
  Independent SDS decoding finds each complete signed PCM sequence in flash.
  State encoding/decoding and a fresh hardware instance preserve those contents;
  each state payload is 1,179,716 bytes.
- A generated two-sample bank (25,468 bytes: 16-bit/5,201 words and 12-bit/4,097
  words, different waveforms and device IDs) imports with zero retries. Both
  full PCM sequences survive state reload. The first sample's playback matches
  its expected waveform with correlation approximately 0.99 before and after
  reload. State payload: 1,310,804 bytes.
- A generated mixed file adds a real firmware-sourced kit after that bank
  (26,701 bytes total). After the loading pause, a new firmware dump request
  confirms its changed kit name, and both sample contents survive reload. Its
  stored-kit fixture can change routing/mutes, so the separate bank test—not
  this mixed fixture—is the playback oracle.
- The firmware cancellation test stops during a sample, verifies a cancelled
  terminal state and idle MIDI ingress, then successfully imports a fresh
  complete sample and verifies its PCM, playback, and state reload.
- The ordinary public single MD kit still imports (1,233 bytes, 311 patch-RAM
  bytes changed). MM `8 CHOR-EXT.syx` still imports (449,728 bytes, 373,652
  user-flash bytes changed), including its 3,145,756-byte state round trip.

The firmware assertions inspect actual decoded sample contents, not merely
"some flash changed". The generated samples are repository-owned test data.
The transport suite independently covers missing/truncated/extra packets,
bad XOR, invalid headers/data bytes, packet wrap, retries, WAIT, wrong peer or
packet replies, CANCEL, overflow, cancellation/drain ownership, mixed-file
pauses, and stale resume actions.

Reproduction (supply legally obtained MD ROM and the matching initialized
factory cache):

```sh
cmake -S . -B temp/build-sysex-investigation \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -Dgearmulator_BUILD_JUCEPLUGIN=OFF -DBUILD_TESTING=ON
cmake --build temp/build-sysex-investigation \
  --target mdSdsTransferTest mdTurboMidiUnitTest mdSdsFirmwareTest mdStateTest
ctest --test-dir temp/build-sysex-investigation --output-on-failure \
  -R '^(mdSdsTransferTest|mdTurboMidiUnitTest|mdStateTest)$'

# ROM-free validation of downloaded files, using the actual preparation gate:
temp/build-sysex-investigation/source/elektron/md/mdLibTest/mdSdsTransferTest \
  --validate-md /path/to/sample.syx /path/to/kit.syx
# Use --validate-mm for Monomachine files.

temp/build-sysex-investigation/source/elektron/md/mdLibTest/mdSdsFirmwareTest \
  /path/to/MD-1.63.bin --generated-bank /path/to/md-uw-1.63-factory-v2.cache
```

Replace `--generated-bank` with a sample `.syx`, `--generated-mixed`, or
`--generated-cancel`. The last mode cancels during a sample, verifies terminal
state and drained ingress, then requires a fresh complete import, persistence,
and playback. `MD_SDS_PANEL_PROBE=/path/to/panel.pgm` optionally records the
front panel during generated-audio verification.

## Remaining release checks and deliberate exclusions

- Run the Windows/Linux/macOS universal and plugin-format CI matrix. The local
  shared-code builds are not a claim of fresh bundle signing or host smoke tests.
- Physically connected MD UW hardware and different historical/custom firmware
  have not been tested. Header recognition is not an exhaustive historical
  firmware-compatibility guarantee.
- MM mixed receive-screen transitions have deterministic transport coverage;
  a complete interactive mixed-file UI session remains a release smoke check.
- Firmware updates, arbitrary controls, SDS extensions, open-loop sending, WAV
  conversion, `.c6` manifests, and receiving/exporting banks are separate features.
- Obtain the alpha tester's rejected file and exact error to close the original
  report conclusively.

Protocol references: [Elektron MD OS 1.63 manual](https://www.elektron.se/wp-content/uploads/2024/09/machinedrum_manual_OS1.63.pdf),
[Elektron MM manual mirror](https://www.bhphotovideo.com/lit_files/85386.pdf),
[MIDI Association universal-message registry](https://midi.org/midi-1-0-universal-system-exclusive-messages),
and the primary [Elektroid SDS implementation](https://github.com/dagargo/elektroid/blob/master/src/connectors/sds.c).
Detailed Elektron dump-format source links remain in the investigation note.
