# MD/MM SysEx format coverage and rejected-sample investigation

Prepared 2026-09-08 in the reused
`gearmulator-md-mm-sysex-sender-v2` worktree at main revision `6ebd2bbc`.
The user-data validator was also compared with
`origin/release/md-mm-alpha` revision `89178c24`; that file was identical at
the two revisions. This note records investigation evidence and a proposed
recognition boundary **before SDS support was implemented**. References below
to the "current" validator describe that baseline. For the implementation and
subsequent emulator-backed acceptance results, see
[SDS sender implementation and acceptance](md_mm_sysex_sds_implementation.md).
Neither note claims verification on physical hardware.

## Tester report and conclusion

An alpha tester reported that many error-free SysEx files were denied,
especially "single banks", while most full snapshots worked. The report is
reproducible when the rejected files are Machinedrum UW samples:

- A full snapshot has no special outer header. It is a concatenation of
  ordinary global, kit, pattern and song dump messages, all of which the
  current validator admits.
- Machinedrum UW samples use MIDI Sample Dump Standard (SDS). SDS messages
  begin `f0 7e`, not Elektron's manufacturer envelope
  `f0 00 20 3c`. The current validator therefore reports valid samples as
  `InvalidFraming` before the firmware sees them.
- Public single-kit, pattern/kit/song, factory-backup and Monomachine DigiPRO
  files passed. Rejection of an ordinary single kit, pattern or DigiPRO bank
  was not reproduced.

The current error text, "not a complete Machinedrum/Monomachine SysEx
stream", is incorrect for the reproduced SDS files. They are complete,
checksum-valid SysEx streams whose protocol is not covered by the current
sender.

One rejected tester file plus its exact UI error is still needed to prove
that the tester's phrase "single banks" means SDS/sample-bank material and
not a second unsupported command family.

## Baseline implementation boundary

`mdsysextransfer.h` accepts only complete messages with this envelope:

```text
f0 00 20 3c <product> 00 <command> ... f7
```

`product` must be `02` for Machinedrum or `03` for Monomachine. The only
admitted dump commands are:

| Model | Admitted commands |
| --- | --- |
| Machinedrum | `50` global, `52` kit, `67` pattern, `69` song |
| Monomachine | `50` global, `52` kit, `5d` DigiPRO, `67` pattern, `69` song |

Every admitted message is checked for complete framing, 7-bit data,
14-bit Elektron checksum and declared length. Commands `7e` and `7f` are
classified as firmware updates. Other correctly framed product commands are
classified as unsupported.

The UI invokes this validator before starting the transport. The transport's
receive parser only retains generic Elektron TurboMIDI responses beginning
`f0 00 20 3c 00 00`; it discards universal SDS replies. During its payload
phase it drains bytes continuously at the negotiated line rate without an
SDS packet/handshake state machine.

## Public corpus and reproducibility

Downloaded inputs were kept under the ignored directory
`temp/sysex-web-corpus-20260908` and are not committed. Sources inspected or
downloaded were:

- Elektron Machinedrum OS 1.63 manual:
  <https://www.elektron.se/wp-content/uploads/2024/09/machinedrum_manual_OS1.63.pdf>
- Monomachine OS 1.32 manual mirror:
  <https://www.bhphotovideo.com/lit_files/85386.pdf>
- Elektron MD SysEx format revision 0.9 mirror:
  <https://usercontent.cdn.cycling74.com/58ed0576285705c15ccfce5c/2025-03-06T20%3A30%3A06Z/MD-SPS1_SysEx_v0.9.pdf>
- Elektron MM SysEx format revision 0.6 mirror:
  <https://usercontent.cdn.cycling74.com/5ac4d6e87351e9739dd2acb7/2021-11-17T09%3A54%3A51Z/mm1_22_sysex_0_6.pdf>
- Archived original Machinedrum factory material and Elektron sound packs:
  <https://techno-id-archives.lebibliophage.com/machinedrum/index.html>
- Public single Machinedrum kit:
  <https://github.com/antonelse/MD-SPSI-MKII-ae>
- Public Machinedrum pattern/kit and liveset dumps:
  <https://www.elektronauts.com/t/my-new-machinedrum-only-album-and-liveset/163652>
  and
  <https://www.elektronauts.com/t/my-second-machinedrum-only-album-and-liveset/171982>
- Public 24-bank Monomachine DigiPRO pack:
  <https://www.elektronauts.com/t/monomachine-digipro-chord-banks-download/210111>
- Elektroid's independent SDS implementation:
  <https://github.com/dagargo/elektroid/blob/master/src/connectors/sds.c>
- MIDI Association universal SysEx registry:
  <https://midi.org/midi-1-0-universal-system-exclusive-messages>
- Representative unofficial MD X.04 OS-update archive:
  <https://github.com/jmamma/MIDICtrl20_MegaCommand/releases/download/3.10/Machinedrum_SPS1-UW_OS_X.04.zip>

The diagnostic classifier included the production validator directly. Its
independent SDS path required a 21-byte dump header, an optional 13-byte MD
sample-name message, sequential packet numbers modulo 128 and 127-byte data
packets. Each packet checksum was independently recomputed as the XOR of
bytes 1 through 124, masked to seven bits, and compared with byte 125.

### Classification results

| Corpus | Files | Result |
| --- | ---: | --- |
| MD single kit, pattern/kit bundles, livesets, sound-pack bundles and factory backups | 8 | 8 accepted |
| MM DigiPRO banks, 64 messages per bank | 24 | 24 accepted |
| MD UW SDS sample files | 2 | 2 rejected by the product validator; 2 valid by independent SDS validation |
| SysEx for a different Elektron product (`0a`) | 1 | Correctly rejected as wrong model |

Selected reproducible inputs:

| File | Bytes | SHA-256 | Observed structure/result |
| --- | ---: | --- | --- |
| `md-single-kit-distorted.syx` | 1,233 | `94aca5cef3e0e460900aa6dd7b725dd1969e2e85363f84c69b080233526f0684` | One MD kit; valid |
| `md-d1516-track.syx` | 12,053 | `e235edbd0bb3a15487d8d4f193c6ca0a420bca6c99d8a0d89489f1120959a3f3` | One kit plus two patterns; valid |
| `md-hemmelig-liveset-202111.syx` | 160,922 | `87dd8e04f5b3d8e31f97ea63a85c770e24d7cb8049ea913bf02cd305181714c5` | 36 MD messages; valid |
| `md-hemmelig-liveset-202204.syx` | 136,254 | `da9b88bad24acd06bbd1c2f69727e416d2dd76d0e4b9a8d5778201a3a3db4781` | 33 MD messages; valid |
| `DUB1_SONG_PAT_KIT.syx` | 24,209 | `cb8b7a698dff072a9f432da491bcae5665736fa3e338c3aa8be90f93a46f50ef` | Two kits, four patterns and one song; valid |
| `RNB1_SONG_PAT_KIT.syx` | 25,442 | `8ae7e0886262aa9c5e0ef175263db05ce577b13d8126223ae51fb1e464ef7f79` | Three kits, four patterns and one song; valid |
| `MD_presets_1_0.syx` | 436,872 | `b6b5516a0e2ed9c219b5878fd2707852fc13de085faa2055e28320dfd44886b2` | 232 MD messages; valid |
| `MD_presets_1_50.syx` | 436,296 | `c1d86de215ceeb1b95843a996c08496534224c6493d5b744b00e4ffacaed1a43` | 232 MD messages; valid |
| `01_DUB1_BD01.syx` | 65,693 | `8229bd00e6e8bca3ecddd996f89e3f9c58eb13de11cd0f9d23953d8cc941f79f` | SDS header, MD sample name, 517 valid packets; rejected as invalid framing |
| `01_RNB1_BD01.syx` | 24,291 | `eaadc7fd95c1cc9314a5093f18a67626ab3e24fd3ede8043168191338e976b46` | SDS header, MD sample name, 191 valid packets; rejected as invalid framing |
| `elektroid-elektron-pattern-86-bpm.syx` | 31,613 | `e01f4570f2d305d67ff36c7668545c9c2b099cc2110e624c323e3c122be8373d` | Elektron product `0a`; wrong model |

The 24-bank MM archive had SHA-256
`e6d4ebbb1d313bf7f60cdab48307c48c1b25b2d1ef0d6e5aa38812c930732356`.
Each extracted file was 449,728 bytes and contained 64 valid `5d` messages.

### Native and firmware-backed checks

A clean macOS arm64 Release build was configured in the ignored directory
`temp/build-sysex-investigation`. `mdTurboMidiUnitTest` passed.

With a locally supplied, legally obtained MD 1.63 ROM,
`mdUserSysexFirmwareTest` rejected `01_DUB1_BD01.syx` before firmware with:

```text
SysEx validation failed: invalid framing
```

The same harness imported the companion `DUB1_SONG_PAT_KIT.syx` at negotiated
8x speed. It changed 10,725 patch-RAM bytes, changed no flash bytes and
completed in 2.203 seconds. This establishes a working control case through
the same validator, transport and emulated firmware.

The inspected X.04 OS file had SHA-256
`5fe9198dbb53cb16bbdf762544adba043e27fde4d9b40838b4df8d2cf1f4070c`.
It contained 15,277 model-specific command-`7e` records followed by one
command-`7f` terminator, confirming the existing firmware-update classifier
against a real update stream.

## Exhaustive recognition families

This inventories the protocol families in the cited official manuals and
detailed format documents relevant to this sender. The detailed format
documents describe older OS revisions; this is not a complete historical
version/revision compatibility matrix. It cannot be an exhaustive list of commands
added by undocumented or third-party firmware. The recognizer should therefore
identify a known envelope before interpreting its command and preserve an
`unknown Elektron command` result.

| Envelope | Meaning | File-sender treatment |
| --- | --- | --- |
| `f0 00 20 3c 02 00 <cmd> ... f7` | Machinedrum product message | Interpret MD command |
| `f0 00 20 3c 03 00 <cmd> ... f7` | Monomachine product message | Interpret MM command |
| `f0 00 20 3c 00 00 <cmd> ... f7` | Generic Elektron TurboMIDI | Transport-internal, not a file payload |
| `f0 7e <device> <sub-id> ... f7` | Universal non-real-time | Interpret SDS subtype when applicable |
| `f0 7f <device> ... f7` | Universal real-time | Valid framing but unsupported control |
| Any other manufacturer ID | Other product | Wrong model/unsupported manufacturer |

Unknown model-specific commands must not be described as malformed. This also
keeps classification truthful for undocumented commands without sending them
automatically.

## Importable Elektron dump signatures

The current detailed format documents show these command, version and revision
bytes immediately after the six-byte product envelope:

| Payload | Header through revision |
| --- | --- |
| MD global | `f0 00 20 3c 02 00 50 06 01` |
| MD kit | `f0 00 20 3c 02 00 52 04 01` |
| MD pattern | `f0 00 20 3c 02 00 67 03 01` |
| MD song | `f0 00 20 3c 02 00 69 02 02` |
| MM global | `f0 00 20 3c 03 00 50 03 01` |
| MM kit | `f0 00 20 3c 03 00 52 02 01` |
| MM DigiPRO waveform | `f0 00 20 3c 03 00 5d 01 01` |
| MM pattern | `f0 00 20 3c 03 00 67 05 01` |
| MM song | `f0 00 20 3c 03 00 69 02 01` |

These are signatures from the cited format documents, not a safe version whitelist.
Older firmware emitted earlier revisions, and the available format documents
do not provide a complete historical compatibility table. Initial recognition
should use product and command, structurally validate the shared footer, and
allow the emulated firmware to decide whether it accepts an older revision.

## Machinedrum SDS grammar

A raw, importable MD sample file observed in the corpus has this structure:

```text
sample-dump := sds-header [md-sample-name] sds-data-packet+
file        := sample-dump+
```

| Message | Bytes | Treatment |
| --- | --- | --- |
| SDS dump header | `f0 7e 00 01 ... f7` | Admit at start of an MD sample dump |
| SDS data packet | `f0 7e 00 02 <packet> <120 data> <xor> f7` | Admit and validate in sequence |
| SDS dump request | `f0 7e 00 03 <sample-lsb> <sample-msb> f7` | Control, not an import payload |
| MD sample-name extension | `f0 00 20 3c 02 00 73 <sample> <name x4> f7` | Admit alone or between SDS header and data |
| Universal EOF | `f0 7e 00 7b <packet> f7` | Generic control identifier; not required for ordinary MD SDS |
| SDS WAIT | `f0 7e 00 7c <packet> f7` | Inbound handshake/control |
| SDS CANCEL | `f0 7e 00 7d <packet> f7` | Inbound handshake/control |
| SDS NAK | `f0 7e 00 7e <packet> f7` | Inbound handshake/control |
| SDS ACK | `f0 7e 00 7f <packet> f7` | Inbound handshake/control |

The MD manual states that the Machinedrum UW is always SDS device `00` when
receiving. MIDI SDS permits other device IDs. The implemented importer validates
the original stream and retargets its SDS device IDs and packet XORs to `00`;
it does not assume MD can receive directly at another ID.

The MIDI Association also defines Sample Dump Extensions under
`f0 7e <device> 05 <extension> ... f7`. Extension IDs `01` through `07` cover
loop-point transmission/request, sample-name transmission/request, extended
dump headers and extended loop-point transmission/request. The MD manual only
promises basic SDS plus Elektron's product-specific command `73`; recognize
standard extensions as SDS but initially report them as unsupported until
firmware behavior is demonstrated.

## Complete documented product command inventory

Only dump rows marked **admit** belong in the ordinary Elektron user-data
path. All other rows should be recognized as control, unused, unknown or
firmware traffic rather than corrupt framing.

| Command | Machinedrum | Monomachine | Sender classification |
| --- | --- | --- | --- |
| `50` | Global dump | Global dump | **Admit** |
| `51` | Global request | Global request | Control |
| `52` | Kit dump | Kit dump | **Admit** |
| `53` | Kit request | Kit request | Control |
| `54` | Unused | Unused | Unsupported |
| `55` | Set current kit name | Set current kit name | Control |
| `56` | Select active global | Select active global | Control |
| `57` | Load pattern | Load pattern | Control |
| `58` | Load kit | Load kit | Control |
| `59` | Save kit | Save kit | Control |
| `5a` | Set MIDI-note map | Unused | Control/unsupported |
| `5b` | Assign machine | Assign machine | Control |
| `5c` | Set track routing | Set track routing | Control |
| `5d` | Set delay parameter | DigiPRO dump | MD control; MM **admit** |
| `5e` | Set reverb parameter | DigiPRO request; manual also duplicates a reverb setter here | Control |
| `5f` | Set EQ parameter | Unused | Control/unsupported |
| `60` | Set dynamics parameter | Unused | Control/unsupported |
| `61` | Set tempo | Set tempo | Control |
| `62` | Set LFO | Unused | Control/unsupported |
| `63` | Unused | Unused | Unsupported |
| `64` | Reset MIDI-note map | Unused | Control/unsupported |
| `65` | Set trig group | Unused | Control/unsupported |
| `66` | Set mute group | Unused | Control/unsupported |
| `67` | Pattern dump | Pattern dump | **Admit** |
| `68` | Pattern request | Pattern request | Control |
| `69` | Song dump | Song dump | **Admit** |
| `6a` | Song request | Song request | Control |
| `6b` | Set receive position | Unused | Control/unsupported |
| `6c` | Load song | Load song | Control |
| `6d` | Save song | Save song | Control |
| `6e` | Not documented | Unused | Unknown/unsupported |
| `6f` | Not documented | Unused | Unknown/unsupported |
| `70` | Status request | Status request | Control |
| `71` | Set status | Set status | Control |
| `72` | Status response | Status response | Control/response |
| `73` | Set UW sample name | Unused | MD SDS adjunct only/unsupported |
| `74`-`7d` | Undocumented | Undocumented | Unknown/unsupported |
| `7e` | OS-update record | OS-update record | Firmware update; reject |
| `7f` | OS-update terminator | OS-update terminator | Firmware update; reject |

The duplicate Monomachine `5e` assignment is present in the published manual.
It does not affect the file-import boundary because neither interpretation is
a dump payload. Any future control-message implementation must resolve the
firmware's payload-length behavior rather than silently choosing one meaning.

Generic Elektron TurboMIDI uses commands `10` through `17` under product ID
`00`: speed request, speed answer, negotiation, acknowledgement, two speed
tests and their results. Those messages belong exclusively to transport
negotiation.

## Recommended implementation boundary

Do not merely relax the first-header check and feed SDS bytes into the current
continuous payload loop. A complete implementation should:

1. Split the input into complete SysEx messages and classify each envelope.
2. Preserve the current checksum/length validation for Elektron data dumps.
3. Validate an SDS stream as a stateful sequence: header, optional MD name,
   sequential packet numbers modulo 128, fixed packet size and XOR checksum.
4. Accept concatenated Elektron dump messages for snapshots and concatenated
   SDS sample dumps for sample banks. Mixed user-data streams can also be
   supported with explicit protocol/readiness boundaries; reject an ordinary
   dump interrupting an incomplete sample, not all mixed files categorically.
5. Add protocol-aware SDS transport handling for ACK, NAK, WAIT and CANCEL,
   including retransmission. Ordinary SDS completion follows the declared
   sample length; the generic universal EOF identifier is not evidence that
   MD SDS requires an EOF handshake. Any open-loop mode needs a separate,
   deliberate compatibility policy, not silent fallback after missing ACKs.
6. Keep OS-update records excluded from this user-data path.
7. Report correctly framed but unsupported commands as unsupported, not
   malformed.

A useful classification vocabulary would be:

```text
Malformed
WrongModel
ImportableUserDump
ImportableMdSds
ControlMessage
SdsHandshake
SdsExtensionUnsupported
FirmwareUpdate
TurboMidi
UnknownElektronCommand
OtherUniversalMessage
OtherManufacturer
```

## Remaining evidence gaps

- Obtain one rejected alpha-tester file and exact popup text.
- Exercise SDS against MD firmware after a protocol-aware sender exists; the
  current firmware harness cannot pass the validation gate for these files.
- Verify closed-loop retry, WAIT, cancellation and timeout behavior in the
  emulator, then on physical MD UW hardware if available.
- Test multi-sample banks, packet-number wrap and truncated/corrupt SDS
  variants.
- Decide deliberately whether any standard Sample Dump Extensions are
  accepted after firmware tests.
- Retest the complete matrix on Windows x64, Linux and macOS universal builds.
- Treat undocumented/custom-firmware commands as an extensibility concern,
  not as license to send unknown control messages automatically.
