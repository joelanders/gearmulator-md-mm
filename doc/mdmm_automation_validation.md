# MD/MM automation validation

The automation controller treats three kinds of state differently:

- A numbered firmware Kit dump is the stored slot. It is authoritative on first
  synchronization and after a Kit selection/reload, but it must not replace
  already-observed live values during a same-slot refresh.
- DAW, UI, and observed MIDI changes are live session state. They remain pending
  until delivered and are persisted even if firmware has not finished booting.
- Mutes are live session/project state because Elektron Kit dumps do not contain
  them. A Kit refresh retains mutes; project state saves and restores them.

## Automated gates

| Gate | Firmware | What it proves |
| --- | --- | --- |
| `mdAutomationMidiTest` | No | MD/MM CC maps, SysEx validation, stored-Kit save/request messages, deterministic dump correlation, and lost-status/dump retry ordering |
| `mdAutomationParameterTest` | No | Stable parameter contract and immediate atomic host-value publication |
| `mdAutomationArchitectureTest` | No | Allocation-free host writes, pending pre-boot state, stale/wrong-slot dump rejection, overflow recovery, restore validation, same-slot live-state retention, and mute persistence |
| `midiOutputDispatcherTest` | No | A blocked/slow physical MIDI sink cannot block realtime producers; saturation, replacement, shutdown, and concurrent producers are bounded |
| VST3 `pluginTester -automation-smoke` | No | Wrapper-visible IDs, repeated automation writes, state save, mutation, and exact state restore |
| `mdAutomationFirmwareTest` | MD + MM | Explicit boot readiness, UART delivery, real firmware Kit save/dump values, bulk writes, and DAW-state replay |
| `mdAutomationRobustnessTest` | MD + MM | Boot races, MIDI NONE replay, external changes, status/dump correlation, malformed/truncated state, and randomized lifecycle transitions |
| `mdAutomationSoakTest` | MD + MM | Sustained writes and concurrent MD/MM instances without firmware MIDI overflow or cross-instance contamination |
| `MD/MM automation core / fixture-free (asan-ubsan)` | No | The fixture-free gates under AddressSanitizer and UndefinedBehaviorSanitizer |
| `MD/MM automation core / shared controller compatibility` | No | Virus, N2x, Vavra, Xenia, and JE-8086 controllers still compile against the changed shared `Parameter`, `Controller`, `Processor`, MIDI parser, and MIDI-output APIs |
| `Elektron macOS universal artifacts` | No | Universal VST3 wrapper automation smoke, AU build/sign/plist validation, fixture-free tests, and packaging checks |
| `Elektron Windows artifacts` | No | Windows VST3 wrapper automation smoke plus fixture-free tests and packaging checks |

The two `fixture-free` jobs, `shared controller compatibility`, and both
platform artifact workflows should be configured as required pull-request
checks. Release tooling must run with both pinned firmware images;
`MD_AUTOMATION_REQUIRE_FIRMWARE=1` makes a missing image a test failure rather
than a skip. `scripts/macos/build_mdmm.sh` enables this by default and runs the
firmware, robustness, and soak gates before deleting its private staged copies.

## Minimal human smoke matrix

Run this only after the automated gates pass. It targets host/device integration
that a headless process cannot faithfully reproduce.

| Platform/host | Wrapper | Product | Actions | Pass condition |
| --- | --- | --- | --- | --- |
| macOS / one supported DAW | AU | MD, MM | Automate one Level lane, stop/start transport, save, close, reopen | UI and sound follow automation; restored value is exact; no startup rollback |
| macOS / one supported DAW | VST3 | MD, MM | Repeat the same value, switch Kit, undo/redo, offline bounce | Repeated point is delivered; selected Kit becomes authoritative; bounce matches realtime |
| Windows / one supported DAW | VST3 | MD, MM | Record knob automation, save/reopen, render faster than realtime | Recorded lane, restored state, and offline render agree |
| Either platform with physical MIDI | Native wrapper | MD or MM | Select a deliberately slow/blocked MIDI output and play dense automation | Audio remains responsive; queued automation catches up after the output resumes |

Record the DAW/version, sample rate, block size, firmware hashes, and result for
each row. Any failure blocks release even when the headless suites pass.
