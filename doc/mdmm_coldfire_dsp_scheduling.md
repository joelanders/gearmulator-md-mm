# MD/MM ColdFire and DSP scheduling optimization

Work on `perf/mdmm-coldfire-scheduler-stepping` (September 2026) reducing MD/MM
CPU usage by roughly a quarter, plus one behavioural change and one bug fix that
came out of validating it. Measured on an Apple M1 Pro with Machinedrum OS 1.63.

## Results

`pluginTester` rendering 60 s of audio through the MD VST3 at 48 kHz with
256-sample blocks, CPU seconds (lower is better):

| Build | CPU |
| --- | --- |
| `release/md-mm-alpha`, plain Release | 45-47 s |
| branch, plain Release | 36.9-37.1 s |
| branch, ThinLTO + `OPTIMIZE_DSP` | ~39 s (measured before the last two commits) |
| branch, ThinLTO + arm64 PGO | 33.2-33.4 s |

The build configuration matters as much as the code: ThinLTO with the DSP
libraries included is worth about 2%, and profile-guided optimization a further
10%. See `mdmm-apple-optimization.md` for the procedure.

## Where the time went

Leaf-sample profile of the audio thread before the work, from `sample(1)` on a
headless render:

| Bucket | Share |
| --- | --- |
| DSP56303 peripheral C++ (ESSI clock, HDI08, DMA, timers) | 30% |
| MD scheduler glue around the UC | 26% |
| Musashi ColdFire core | 15% |
| DSP JIT code | 15% |
| ColdFire exec wrapper + `md::Sim` | 7% |
| ColdFire memory map (`resolve`, `read16`, `write16`) | 5% |

The ColdFire interpreter is not the bottleneck. In isolation the MCF5206E core
runs at about 132 M instructions/s, roughly 6.6x faster than the 40 MHz part it
emulates. What cost time was the per-instruction work wrapped around it, and the
rate at which the DSP peripheral chain was being polled.

A PC histogram of the firmware showed ~1200 distinct hot addresses with the
hottest at 3.5%: MD 1.63 is compute-bound in steady state, so the existing
idle-skip path (`Microcontroller::idleSelfBranchInstructions`) almost never
engages and cannot be the lever.

## Changes

### `bcb626fb` — per-instruction overhead around the ColdFire

Four independent changes in `mdLib`:

**Batched UC slice.** `Hardware::schedStep` ran its quantum as a loop calling
`processUC()` once per instruction. Each call re-read the project-restore flag,
the panel queue, three MIDI queue states, the transfer wire ownership and the
host-pump dirty flag, then compared progress with a double-precision divide.
`Hardware::runUcSlice` now runs the whole quantum in one loop and samples
producer-published state once per slice. This is safe because every producer
either runs on the scheduler thread or is serialized against rendering by the
Plugin device lock, so a change can only arrive between slices.

State this thread changes itself is still re-evaluated per instruction: queue
pops, SysEx transfer service, and in particular the HREQ-to-IRQ4 host pump,
which must stay current because inline DSP catch-up during an instruction can
raise it. `processUC()` keeps its exact single-step semantics for the tests that
drive it directly (`midiTimingTest`, `hostRxFirmwareTest`, `idleSchedulerFirmwareTest`).

**Deadline-driven `md::Sim`.** `Sim::exec` stepped two timers and the panel UART
shift register on every instruction. It now accumulates cycles and applies them
only when the accumulated count reaches the nearest event, or when a register
access observes the state. `cyclesUntilNextTimerInterrupt` and
`cyclesUntilNextUartTransmit` subtract the pending count, so the idle-skip path
and the standalone UART tests see identical values.

**Memory-map page table.** `Microcontroller::resolve` walked a linear chain of
range checks on every data access, and patch-RAM accesses took a
`std::shared_mutex` — on the hot path, since the firmware's inner loops read
patch RAM. A two-level 64 KiB page table now resolves RAM, aliases, internal
SRAM and (while the flash command decoder is passive) ROM directly, with
`*Slow` variants behind it for peripherals and unmapped space. Flash pages are
unmapped only while the decoder intercepts reads, tracked by the new
`FlashCommandDecoder::interceptsReads`. The mutex remains for whole-image copies,
which all run under the device lock.

**Inlined `TurboMidiTransfer::ownsMidiWire`**, polled at instruction granularity.

### `19ffd1fa` + dsp56300 `b76c02ea` — inlined peripheral due test

The `exec8times` JIT trampoline already inlined `DSP::execPeriph`'s "is a
peripheral due" test, but `execUntilCycles` — the entry that cycle-bounded hosts
like MD/MM use — called the peripheral function pointer before every block
unconditionally. The test is now inlined there too on ARM64 and x86-64,
extended with the cycle-domain deadline those hosts use.

To make it two unsigned compares, "no cycle deadline" is now the sentinel
`IPeripherals::NoCycleDeadline` in `m_targetCycle` rather than a separate flag.
`HDI08::exec` also samples HCR and the receive queue once per tick instead of on
every predicate, and `EsxiClock::usesExactCycleDeadline` is inlined.

### `1fefca3f` — catch-ups through the trampoline

`schedCatchUpDsp` and `schedCatchUpDspToDsp` stepped the DSP one JIT block per
C++ `exec()` call; the PGO profile counted 1.1 billion such calls per ~80 s of
audio. For the Machinedrum they now use one `execUntilCycles` entry with the
same stop condition. The Monomachine keeps the per-block loop because its
host-transmit back-pressure gate has to be evaluated between blocks.

### `ff633dfb` — Machinedrum ESSI in the cycle domain

**This is the one change that alters rendered audio.** See the equivalence
section below before deciding whether to keep it.

The ESSI clock counts DSP cycles, but the peripheral chain is scheduled in
instructions, and `EsxiClock::exec` halves the cycles-until-next-slot to convert,
assuming two cycles per instruction. MD firmware retires about 1.2 cycles per
instruction, so each 96-cycle link slot was scheduled early and then
re-approached at 19, 9, 4, 2, 1 and 0 instructions — six or seven chain ticks
per slot. The PGO profile measured 6.8 M ticks/s against 2.1 M actual link
slots/s.

`exactEssiCycleDeadlines` in `mdtransportpolicy.h` is now true for the MD, as it
has been for the MM all along. Slots are serviced at their exact boundary.
Worth about 10% of the audio thread.

### `f75166c1` — frame-domain slice predicate

A bug in the batched slice, found by the audio-equivalence check below. The
slice stopped at an integer target of `ceil(subTarget * ucPerFrame)` instead of
the fractional predicate `done / ucPerFrame < subTarget` that the
per-instruction loop used. Floating-point rounding makes the two differ by one
instruction at some slice boundaries, which shifts the ColdFire/DSP interleave
and changes rendered audio — trigger timing moved by tens of milliseconds.
Restoring the original predicate costs about 5% of the speedup and is not
optional: without it the branch is not timing-transparent.

## Audio equivalence

Validated by building `release/md-mm-alpha` in a separate worktree and rendering
a fixed 16 s sequence of pad hits from a saved kit through both trees, comparing
sample by sample. Both renders are deterministic, so any difference is real.

- The batched slice (with `f75166c1`), deadline-driven `Sim`, page table, DSP
  trampoline check and trampoline catch-ups are **bit-identical** to the
  pre-branch code. Confirmed by reverting each sub-change in turn.
- The exact ESSI cycle deadline is **not**. In that test the first triggered
  sound started about 24 ms later — roughly one control tick — and sample
  content differs from there.

What the ESSI change moves is *when within a link slot* the two DSPs exchange
words. The UC clock, SIM timer cycle counts and the 2304-cycle DSP frame are
unchanged, so modulation and sequencer rates are not affected; what shifts is
the phase at which a voice starts relative to the control tick that triggered
it. A retriggered LFO can therefore start on a different tick. There is no
hardware reference recording to say which version is closer to the machine; the
argument for the new one is that it services the link at its exact boundary
rather than approaching it through rounding error.

Reverting is a one-word change in `mdtransportpolicy.h`, at the cost of about
10% of the gain.

## Tried and rejected

**Collapsing pure-NOP DO loops.** DSP2 spends 57% of its time in a `do #$32`
delay loop at `P:$100095`. A JIT fast path to skip its iterations was
implemented and measured: it never skipped anything, because during that loop
DSP2's peripheral deadline is always 0-4 instructions away — the "delay" is the
firmware waiting for DMA and ESSI traffic that is streaming the whole time. The
helper cost about 5% and was reverted.

**Due-driven peripheral sub-chain.** Skipping HDI08/timers/DMA when not due
looked worth ~5%, less than first estimated because most ticks carry a real slot
event, and it needs an audit of every mutator including cross-thread host writes
in other products. Not attempted.

**Poll-loop fast-forward.** DSP2's PDRC poll at `P:$bb` and DSP1's DDR0 poll at
`P:$3c` are the remaining busy-waits (DSP2 is ~99% idle, DSP1 ~49%). DSP2 faces
the same DMA-deadline cap as the NOP loop; DSP1's poll has a 36-instruction link
slot of headroom and remains a genuine opportunity.

## What DSP2's DMA channels do

Recorded while investigating the tick rate, since it is not documented elsewhere:

| Channel | Direction | Configuration |
| --- | --- | --- |
| 0 | Voice audio out to the link | Y:$11e → ESSI0 TX00, word/request, re-armed per frame from the ISR |
| 1 | ADC capture | ESSI1 RX1 → X:$13c/$1bc double buffer, 255 words, continuous |
| 2 | Link in from the mixer DSP | ESSI0 RX0 → X:$740/$7c0, 63 words, request-triggered |
| 5 | Host upload | ColdFire sends address and count over HI08 (`movep x:<<M_HORX,x:<<M_DDR5`), then DMA fills Y memory. Kit and parameter uploads |

All are request-triggered word channels. The one mode that schedules its own
per-word ticks, block-triggered-by-DE, is not used.

## Verification

Gates run on the plain Release and PGO builds:

- `dsp56300_unitTests`, `dsp56300_accumulatorTests` (also on the x86-64 slice
  under Rosetta, which exercises the x86 trampoline path)
- `mc68kColdFireTimingTest`, `mc68kHdi08ReceiveTest`, `mc68kColdFireDivideTest`
- `mdIdleSelfBranchTest`, `mdIdleSchedulerFirmwareTest` — scheduler equivalence
- `mdHostRxTimingTest`, `mdMidiTimingTest`, `mdTransportScorecardTest`
- `mdAudioFirmwareTest`, `mdUwFirmwareTest`, `mdPanelReadinessFirmwareTest`,
  `mdEncoderPressFirmwareTest`, `mdAutomationFirmwareTest`,
  `mdProgramChangeFirmwareTest`, `mdFlashTest`, `mdStateTest`

The MM firmware tests were **not** run — no Monomachine image was available.
The MM shares all of this code except the ESSI policy bit and the two
Monomachine-only back-pressure paths, so it should be exercised before merging.

The PGO bundle was verified to render bit-identically to a plain Release build
of the same source, confirming the profile changes speed only.

## Open items

- Push the branch and the dsp56300 submodule branch
  (`perf/exec-until-cycles-inline-periph-check`).
- Run the MM firmware suite with a Monomachine image.
- One function in `mdhardware.cpp` has a mismatched hash between the
  instrumented and optimized compiles, so the profile-use build needs
  `-Wno-error=profile-instr-out-of-date`. The official release script rejects
  that, so a receipt-clean PGO release needs it resolved.
- DSP1's DDR0 poll fast-forward, per above.

The measurement harness used for this work (a headless `md::Device` driver with
render-dump and comparison scripts) was session-local and is not in the tree.
`pluginTester -plugin <bundle> -seconds N -blocksize B -samplerate S` reproduces
the CPU timings directly.
