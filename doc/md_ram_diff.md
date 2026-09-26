# Machinedrum RAM dump and patch map

A debug window that snapshots ColdFire RAM while the OS 1.63 firmware runs,
highlights bytes that change after panel/SysEx actions, and labels kit, pattern,
song, and global areas. Off by default.

Firmware layout below was probed on Machinedrum OS 1.63 with the same load path
the plugin uses (`RomLoader::findROM` + product `homePath`). Field *shapes*
come from Elektron *Machinedrum System Exclusive description* v0.9; absolute
addresses come from `mdPatchRamMapFirmwareTest`.

## Open the window

1. Right-click the instrument background.
2. **Performance diagnostics** → **Log RAM diffs for panel actions**.

A separate native window opens (`mdRamDiffWindow.rml`). Closing it disables the
probe (`callAsync` → `setRamDiffEnabled(false)`) so the window is not destroyed
on its own click path.

Config key `ramDiffProbe` (bool, default off).

## Window

| Control | What it does |
|---|---|
| Hex / Binary | Byte hex vs bit view. Binary highlights XOR bits. Config `ramDiffBinary`. |
| patch / main / sram / loader | Exclusive region. Only that region is copied, diffed, and painted. Config `ramDiffRegion` (0–3). |
| autoscroll | After a panel/trig action, jump to the first changed row. Config `ramDiffAutoscroll`. |
| prev / next | Step through remaining highlight rows. |
| fade 1–30 s | How long a changed byte stays highlighted. Config `ramDiffFadeSeconds` (default 5). |
| map none / all / pattern / kit / song / glob | Row labels. **all** is default. Pattern/kit/song/glob jump to the *currently selected* record. Config `ramDiffAnnotate` (0–5). |

Status line: last action, changed-byte count, field name, map mode, and

`A01 · kit 01 · song 01 · global 1 @ 0x…`

from GET STATUS (`0x04` / `0x02` / `0x08` / `0x01`) plus the map base.

Dump is a virtualized hex canvas (18 px rows, 16 bytes/row). Only visible rows
paint. Live snapshot is 5 Hz with `memcmp` skip. A 16 dp right-edge scrollbar
shows a thumb and red ticks at highlighted rows. Wheel: `delta.y * 12`. Window
bounds persist as `ramDiffWindowBounds`.

Hex uses the default monospaced face (RmlUi has no generic `monospace` in this
skin; panel Roboto is not mono).

### Map gutter

When a map is on, the left ~180 px of each row shows a colour bar and a label,
and bytes tint with the field colour. Mixed 16-byte rows join both halves, e.g.
`A01 pattern triggers 1-32 tracks 1-4` or `kit 1 T1 synth 1-8 / T1 fx 1-8`.

## Probe timing

Firmware mutates RAM *after* `sendPanelEvent` on the audio thread, so a
same-click after-snapshot is empty. The editor:

1. Copies the selected region under `withDeviceLocked` on press.
2. Waits ~8 panel-timer ticks after release (~260 ms).
3. Diffs, coalesces consecutive changed bytes, and feeds the overlay.

`withDeviceLocked` must not allocate; buffers are pre-sized when the probe is
enabled. Overlay then polls `Editor::captureRamRegion` at 5 Hz on its own.

## ColdFire regions

Writable MCU RAM the dump can show. Flash, SIM, and DSP memory are excluded.

| Kind | Range | Size | Flag |
|---|---|---|---|
| patch | `0x00100000`–`0x00200000` (`g_patchBootstrap`) | 1 MiB | 1 |
| main | `0x00200000`–`0x00300000` (`g_mainRam`) | 1 MiB | 2 |
| loader | `0x00310000`–`0x00400000` (`g_loaderRam`) | 960 KiB | 8 |
| sram | `0x01000000`–`0x01010000` (`g_internalSram`) | 64 KiB | 4 |

Kits, patterns, songs, and globals live in **patch**. Main holds working copies
and UI scratch (trig edits often touch hundreds of main bytes as well as one
patch byte). Copy APIs: `Hardware::copyWorkingRamRegion` / `copyWorkingRam`.
Patch copy takes the factory-flash mutex; main/SRAM need the device lock.

## Patch RAM map (OS 1.63)

Addresses are absolute. Slot *offsets* are unpacked (not SysEx positions: 7-bit
encoding in the PDF inflates those).

```
0x00100000  kit working copy (preamble + name + params …)
0x00100008    current kit number
0x00100012    kit name (16 ASCII)
0x00100022    track 1 synthesis A
…
0x001120ca  global slot 1 (8 slots × 0xBC inferred)
0x001126ac  song 01 working copy (32 songs × 0xA10 inferred)
0x001126ae    song name
0x001272be  current pattern (2 bytes)
0x001272c0  pattern A01  (128 slots × 0x896, in place through H16)
0x00173dcc  end of pattern array (A01 + 128 × 0x896)
```

### Current selection

| What | RAM | GET STATUS |
|---|---|---|
| Current kit | `0x00100008` | `0x02` (0–63) |
| Current global | `0x001120ca` | `0x01` (0–7) |
| Current song | `0x001126ac` | `0x08` (0–31) |
| Current pattern | `0x001272be` (2 bytes) | `0x04` (A01=0 … H16=127) |

Pointer hunt: SET STATUS `0 → 3 → 7` and keep bytes that follow all three
values. Main RAM has extra copies (e.g. `0x0029e9af` is shared scratch; ignore).

### Patterns (8 banks × 16 = 128)

Stored **in place**. GRID RECORD edits the selected slot. No pattern save.

| | |
|---|---|
| Base | `0x001272c0` (A01 trigs) |
| Stride | `0x896` (2198) |
| Count | 128 (`A01` … `H16`) |
| Evidence | A01 T1 LSB `0x001272c3`; A02 `0x00127b59`; B01 `0x0012fc23` |

`0x896` = trigs/locks/accent (`0x90`) + 6 header bytes + 64 lock rows × 32.

Each slot, relative to its base:

| Offset | Size | Window label |
|---|---|---|
| `0x00` + `t*4` | 4 | pattern triggers 1-32 track `t+1` |
| `0x40` + `t*4` | 4 | pattern lock params 1-24 track `t+1` |
| `0x80` | 4 | pattern accent steps 1-32 |
| `0x84` | 4 | pattern slide steps 1-32 |
| `0x88` | 4 | pattern swing steps 1-32 |
| `0x8c` | 4 | pattern swing amount |
| `0x90` | 1 | pattern accent amount |
| `0x91` | 1 | pattern length 1-64 |
| `0x92` | 1 | pattern tempo multiplier |
| `0x93` | 1 | pattern scale 16/32/48/64 |
| `0x94` | 1 | pattern kit number |
| `0x95` | 1 | pattern lock row count |
| `0x96` | `0x800` | pattern parameter locks |

Trig words are big-endian `uint32`. Bit 0 is step 1 in the **last** byte (LSB).
Bits 0–15 are the 16 TRIG keys on page 1; 16–31 are steps 17–32. A 16-byte dump
row therefore covers four tracks: `A01 pattern triggers 1-32 tracks 1-4`.

64-step Extra Pattern 64 (OS 1.50+, 2316 unpacked bytes) is **not** in this
stride.

Naming: bank `A`–`H` = `index / 16`, number `01`–`16` = `(index % 16) + 1`.

### Kit working copy

SET STATUS kit (`0x02`) updates `0x00100008` but does **not** swap the buffer.
LOAD KIT (`0x58`) copies a stored kit into this working copy; SAVE KIT (`0x59`)
writes it back. The dump therefore labels **one** kit (the loaded one), not 64
in-place slots.

| | |
|---|---|
| Preamble | `0x00100000` (18 bytes) |
| Current kit | `0x00100008` |
| Name | `0x00100012` (16 ASCII) |
| T1 synth A | `0x00100022` |

Offsets from the **name** (SysEx `0x52` unpacked):

| Offset | Size | Window label |
|---|---|---|
| `0x00` | 16 | kit name |
| `0x10` + `t*24` | 8 | track `t+1` synthesis parameters 1-8 |
| `0x18` + `t*24` | 8 | track `t+1` effects parameters 1-8 |
| `0x20` + `t*24` | 8 | track `t+1` routing parameters 1-8 |
| `0x190` | 16 | kit track levels 1-16 |
| `0x1A0` | 64 | kit drum models tracks 1-16 (16 × BE `uint32`) |
| `0x1E0` + `t*36` | 36 | track `t+1` LFO |
| `0x420` | 8 | kit reverb parameters 1-8 |
| `0x428` | 8 | kit delay parameters 1-8 |
| `0x430` | 8 | kit EQ parameters 1-8 |
| `0x438` | 8 | kit dynamics parameters 1-8 |
| `0x440` | 16 | kit trig groups tracks 1-16 |
| `0x450` | 16 | kit mute groups tracks 1-16 |

Panel order for the 24 track params: SYNTHESIS A–H, EFFECTS A–H, ROUTING A–H.
A mixed row is `T1 synth 1-8 / T1 fx 1-8`. LFO struct (SysEx v0.9): destination
track, destination param, shape 1, shape 2, type (`Free` / `Trig` / `Hold`),
then 31 bytes of internal state.

### Songs (32)

Working copy at `0x001126ac`. Selecting song 02 replaced `UW DEMO` with
`NEW SONG` at `0x001126ae`. Inferred stride `0xA10` (16-byte name + 256 ×
10-byte rows from SysEx `0x69`).

| Offset | Size | Window label |
|---|---|---|
| `0x00` | 1 | song number |
| `0x02` | 16 | song name |
| `0x12` | `0x9FE` | song rows |

### Globals (8)

Base `0x001120ca`, inferred stride `0xBC` (8 × `0xBC` = `0x5E0` lands on the
song block at `0x001126aa`). Slot 1 index shares the current-global byte.

## SysEx vs RAM

SysEx only allows 7-bit data. 8-bit blobs (trig words, drum models, LFO, lock
rows) are packed: 7 bytes → 8 data. RAM holds the **unpacked** layout. Do not
use PDF SysEx positions as RAM offsets.

Appendix C SET/GET STATUS (`0x71` / `0x70` / `0x72`):

| Param | Meaning |
|---|---|
| `0x01` | current global (0–7) |
| `0x02` | current kit (0–63) |
| `0x04` | current pattern (A01=0 … B01=16 … H16=127) |
| `0x08` | current song (0–31) |
| `0x10` | sequencer mode (pattern=0, song=1) |
| `0x20` | lock mode (classic=0, extended=1) |
| `0x22` | current track (0–15) |

Related dumps: kit `0x52`/`0x53`, pattern `0x67`/`0x68`, song `0x69`/`0x6a`,
global `0x50`/`0x51`. Load kit `0x58`, save kit `0x59`, load pattern `0x57`,
load song `0x6c`.

## Panel sequences (manual OS 1.63)

- GRID RECORD: `[RECORD]` (steady LED), SOUND SELECTION for track, `[TRIG]` to
  toggle a step. More than 16 steps: `[SCALE SETUP]` changes page.
- Kit edit: `[KIT]`, then DATA ENTRY A–H on SYNTHESIS / EFFECTS / ROUTING.
- Scale: `[FUNCTION]` + `[SCALE SETUP]`.
- Patterns are edited in place; there is no pattern save.

## Re-run the firmware probe

Boots like the plugin: empty `romData`, `RomLoader::findROM` from
`Documents/Gearmulator Preview/Machinedrum/roms/` (and the module path),
`homePath` that product folder so `nvram/md-uw-1.63-factory-v2.cache` loads
when present.

```sh
cmake --build cmake-build-debug --target mdPatchRamMapFirmwareTest
./cmake-build-debug/source/elektron/md/mdLibTest/mdPatchRamMapFirmwareTest
```

The test toggles trigs on A01 / A02 / B01 / H16, tweaks kit synth A on kits
01 / 02 / 03 / 64, hunts current-selection pointers (`0 → 3 → 7`), and
switches song 01 → 02. Update `mdRamLabels.h` constants if the printed bases
change.

Unit labels (no firmware): `mdRamDiffTest`.

## Source

| Path | Role |
|---|---|
| `source/elektron/md/mdLib/mdramdiff.h` | Region kinds, copy/diff/format |
| `source/elektron/md/mdLib/mdRamLabels.h` | Field tables, probed bases, `hit` / `annotateRow` |
| `source/elektron/md/mdLib/mdmemorymap.h` | ColdFire ranges |
| `source/elektron/md/mdLib/mdmidiprotocol.h` | SET STATUS select kit/pattern/song/global/track |
| `source/elektron/md/mdJucePlugin/mdRamDiffOverlay.cpp` | Native window, canvas dump, map bar |
| `source/jucePluginData/mdRamDiffWindow.rml` | Toolbar / map bar / canvases |
| `source/jucePluginData/mdRamDiff.rcss` | Window styles |
| `source/elektron/md/mdJucePlugin/mdEditor.cpp` | Enable, settle, `captureRamRegion` |
| `source/elektron/md/mdJucePlugin/mdMcpRamTools.cpp` | MCP `ram_map` / `ram_read` / `ram_write` / `ram_fill` |
| `source/elektron/md/mdLibTest/mdPatchRamMapFirmwareTest.cpp` | OS 1.63 probe |
| `source/elektron/md/mdLibTest/mdRamDiffTest.cpp` | Diff + label unit tests |

## MCP RAM tools

Machinedrum and Monomachine MCP RAM tools are documented separately:

- [md_mcp.md](md_mcp.md) — named pattern/kit/song/global segments
- [mm_mcp.md](mm_mcp.md) — same `ram_*` tools, raw region+address+size

Shared plugin MCP tools remain in [mcp_server.md](mcp_server.md).

## Limits

- Stored kit bank (64 slots) is not an in-place array next to the working copy.
  SET STATUS kit only writes the current-kit byte.
- Song and global strides are inferred from block packing, not from editing
  song 02 / global 2 in isolation.
- Extra Pattern 64 (steps 33–64) RAM offset is unknown.
- Trig edits also churn main RAM (hundreds of bytes); the labelled patch byte
  is the one that matches the SysEx field.
- H16 trig probe did not land on `A01 + 127 × 0x896`; A01/A02/B01 are the
  stride evidence.

## Compile-time option

The entire ramdiff infrastructure — ColdFire region types and diff/format
helpers in `mdLib`, the panel-action probe and native overlay window in the
Juce plugin, the MCP `ram_map` / `ram_read` / `ram_write` / `ram_fill` tools,
the editor `captureRamRegion` delegation, and the `mdRamDiffTest` / firmware
probe tests — is gated by the `GEARMULATOR_MDMM_RAM_DIAGNOSTICS` CMake option.
It defaults to off; enable it with

```sh
cmake -S . -B build \
  -DGEARMULATOR_MDMM_RAM_DIAGNOSTICS=ON
```

When the option is off:

- `md::ramDiff` namespace types, helper functions, and `mdRamLabels` are not
  compiled into `mdLib`.
- `RamDiffProbe`, `RamDiffOverlay`, and `mdMcpRamTools` are not compiled into
  the Juce plugin.
- The overlay RML/RCSS assets (`mdRamDiffWindow.rml`, `mdRamDiff.rcss`) are
  not embedded in the plugin binary.
- `Editor::ramDiffEnabled()` / `setRamDiffEnabled()` / `getRamDiffProbe()` are
  not declared, and all `m_ramDiffProbe.*` call sites in `mdEditor.cpp` are
  compiled out.
- The MCP server registers no `ram_*` tools and exposes no `ramDiff` analyser
  category or menu items.
- `mdRamDiffTest` and `mdPatchRamMapFirmwareTest` are not built.

Leaving it off keeps the diagnostics out of regular builds (e.g. distro
packaging, minimal standalone builds, or third-party forks) where investigating
live RAM layout and panel-action diffs is not wanted.
