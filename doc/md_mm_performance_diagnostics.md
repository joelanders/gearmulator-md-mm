# MD/MM performance diagnostics

Performance capture helps distinguish sustained CPU overload from lock waits,
resampling overhead, JIT events, and work advancing a replacement machine during
project restore. It is off by default and never uploads a report.

## Collect a report

1. Open **Settings → GUI → Performance diagnostics** in Gearmulator MD or MM.
2. Click **Start performance capture**. Wait for the status to say recording.
3. Reproduce the crackling or slow playback using your normal host settings.
4. Click **Stop performance capture**, then **Open logs folder**.
5. Share the `performance-<timestamp>-<unique-id>.jsonl` file together with what
   you were doing and approximately when you heard the problem.

The folder is `logs/` beside the product's `roms/` and `config/` folders, beneath
its public data directory. This also respects `GEARMULATOR_DATA_ROOT`.
Each plugin instance and each capture gets a separate filename. Existing reports
are preserved. Capture automatically stops after ten minutes or 8 MiB. Start a
new capture if you need another recording. Settings display file/directory errors
and automatic stops. The worker flushes approximately every two seconds, so a
crash can lose the most recent interval. Stopping or closing the plugin flushes
the report; a callback still in flight at stop may be absent from the final data.

Capture is session-only: reopening the plugin does not restore the GUI setting.
For hosts without an open editor, set `GEARMULATOR_RT_INSTRUMENTATION=1` (also
accepts `true`/`TRUE`) **before starting the host**. MD/MM then automatically starts
an exported capture for each instance. Remove the environment variable to prevent
this on later launches. The GUI can stop an environment-started capture.

## Contents and interpretation

The file is JSON Lines: every complete line is independently parseable JSON.
Schema 1 contains a `session` header, cumulative `summary` records, selected
`callback` records, and an `end` record (`stopped` or `capture_limit`). Durations
are nanoseconds. Summary elapsed time and callback start time are relative to
capture start; they use a monotonic clock.

The session identifies the product, version/build revision, host/plugin format,
OS, CPU model/vendor, logical CPU count, reported clock speed and process
architecture. Callback records include actual block size and sample rate,
device sample rate, resampler mode, DSP clock percentage, active output layout,
transport/bypass/offline state, and incoming MIDI event/byte counts. No MIDI
payload, notes, audio, presets, project contents, or firmware data are recorded.

The realtime load histogram counts duration divided by the nominal block budget
in six buckets: `<25%`, `25–50%`, `50–75%`, `75–100%`, `100–150%`, `≥150%`.
Offline callbacks are counted separately and excluded from both this histogram
and estimated deadline overruns. Deadline estimates are **not host/driver xrun
notifications**. Host scheduling and buffering can differ from the nominal budget.
Inter-callback spacing is context, not proof that the host scheduled us late.

The callback trace records the first callback, periodic context samples (every
1024 callbacks), realtime callbacks consuming at least 75% of their budget,
lock waits of at least 100 microseconds, callbacks with JIT compilation, and
callbacks advancing both live and restore machines. Each trace entry correlates
those events with the same callback's timings. A fixed 512-entry queue drops new
trace records when full; `slowCallbacksDropped` reports this. Summaries continue
to count all callbacks. A trace is a selected sample, not an exhaustive profiler.

| Observation | Next investigation or comparison |
| --- | --- |
| Most realtime callbacks are near/exceeding budget and device time dominates | Sustained emulation cost. Compare a larger host block, fewer active outputs, and the same project on another CPU. |
| Slow callbacks spend a large share waiting for the synth lock | Inspect concurrent state/settings/controller operations and how long they hold that lock. |
| Resampler time substantially exceeds device time | Compare resampler modes and host/device sample-rate combinations. |
| Spikes coincide with `dualMachine` and substantial `deferredNanoseconds` | Investigate scheduling or moving restore preparation off the audio thread. |
| Spikes coincide with live/deferred JIT counts | Investigate warmup/precompilation for that machine; confirm compilation cost with a profiler. |
| Callback timing is comfortably within budget despite audible trouble | Investigate host/driver scheduling and other processing; the capture does not prove the entire audio system met its deadline. |

Timings are **inclusive**: synth time includes its lock wait and resampler work;
resampler time includes device processing; device time includes deferred-machine
advancement. Do not add these totals together. Resampler minus device time is an
approximation of resampling/dispatch overhead. JIT counts measure code-generation
entries; this feature does not measure compilation duration or split device cost
into MCU/DSP/peripheral phases. Those remain targeted profiling follow-ups.

Snapshots use relaxed atomics and are approximate while recording/resetting.
Individual trace entries have synchronized ownership and are not torn. Measuring
adds some overhead, so compare captures with the same instrumentation settings.
Reported CPU MHz is descriptive and does not track dynamic frequency or throttling.

## Implementation and validation

The audio path records into preallocated storage, with no report formatting,
filesystem calls, worker notification, or waiting for queue capacity. A dedicated
`PerformanceReport` worker is the sole trace consumer and handles formatting,
file I/O, periodic flushes and limits. The processor owns the writer and destroys
it before destroying the synth. Capture stops on open/write/flush failures.

`synthLibPerformanceReportTest` covers correlated phase/context capture, offline
accounting, queue overflow and concurrent draining, actual synth processing at
44.1/48/96 kHz, JSON escaping, output, file errors, automatic limits and rapid
start/stop. `synthLibAudioInstrumentationTest` runs the prepared-audio allocation
regression with capture scopes enabled. These are included in the focused CI gate.
