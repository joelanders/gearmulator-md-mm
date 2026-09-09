# MD/MM plugin timing and latency

Host MIDI offsets previously disappeared before firmware admission, the sequencer clock waited a beat before its first tick, and input FIFOs could retain the time before ADC startup as a permanent unreported delay. Large JIT first-use allocations also caused callback overruns. These are separate problems: host compensation cannot correct an event admitted at the wrong sample or make a slow callback finish sooner.

Incoming MD/MM MIDI now has an absolute native-sample deadline converted upward to an emulated CPU-cycle deadline. A bounded stable queue preserves message ordering and retains due messages while UART or restore state prevents delivery. The scheduler's idle skip stops at the next deadline. MD pad notes become complete panel UART pulses without UI coalescing; MM/general MIDI retains complete messages. Firmware UART output records cycle timestamps, and the resampler/JUCE boundary preserves the resulting output offsets.

Resampled events use absolute 64-bit host/native positions and survive zero or multiple native pulls. Source requests no longer double-count previously buffered output. Clock generation handles Start, Stop, tempo changes and seek/loop boundaries using absolute 24-PPQN ticks; MIDI song-position resolution still limits arbitrary seeks.

Input samples carry native machine positions. Receivers discard expired samples at late startup rather than keeping a boot backlog. Both MM DSPs receive the ADC stream, including the producer that serves tracks 1–3. The explicit 64-native-frame input reserve is retained.

## Compensation contract

Fresh MD/MM instances default to zero additional blocks; saved explicit settings remain respected and now impose a real delay. With the default legacy resampler and zero additional blocks:

| Host rate | Synth-only report | Input-enabled report |
| --- | ---: | ---: |
| 44.1 kHz | 0 | 64 |
| 48 kHz | 32 | 133 |
| 96 kHz | 63 | 255 |

Values are host samples and cover explicit buffering/filter delay. They do not predict every machine's acoustic response. Firmware track processing, routing, envelopes and effects vary; one global report cannot align every different track/patch response. Native-rate rounding can conservatively add a host sample to a converted extra-block setting.

## Validation and scope

The fixture-free clock/resampler, MIDI-queue/parser and input-timeline tests join the existing `synthLibAudioTest`, `mdLibTests` and `mdAudioQueueTest` CTest gates. The new UART tests from the release remain included. The JUCE layout test also verifies outgoing MIDI offsets. Full firmware checks require user-provided `GEARMULATOR_MD_FIRMWARE_BIN` and `GEARMULATOR_MM_FIRMWARE_BIN`; firmware is not distributed here.

Private headless-host captures retain exact plugin/host/data hashes, MIDI/playhead sample positions, callback timing and full audio. Evaluate processing deadlines separately from acoustic onset, and use coded-input correlation for the input path. Use the same patch and controller traffic for comparisons; ordinary message-thread activity can change firmware phase. Instrumented PGO training captures are not performance benchmarks. Regenerate profiles when source, compiler, architecture or relevant settings change.

Known limits: first notes can still exceed small-buffer deadlines, state restore interrupts sound, and an exclusive SysEx file transfer defers clocks. Offline controller traffic differs from ordinary message dispatch, so completed offline renders are not promised bit-identical to realtime renders. Live itself has not been validated; playback compensation and live monitoring require separate checks. The input report is not a physical-interface round-trip measurement.

## Dependency order

The linked DSP PR supplies sparse metadata and macOS copy-on-write dispatch templates. The parent draft uses its published candidate commit only to make review/CI reproducible. Merge the DSP PR into its intended release first, then replace the provisional parent pin with that resulting merged release revision and validate the actual combination before merging the parent. These PRs do not install plugin binaries or publish ROMs, NVRAM, captures or optimization profiles.
