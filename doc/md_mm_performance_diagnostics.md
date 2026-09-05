# MD/MM performance diagnostics

Performance capture helps distinguish sustained CPU overload from lock waits,
resampling overhead, JIT events, and work advancing a replacement machine during
project restore. It is off by default and never uploads a report.

## Collect a report

1. Right-click the instrument background in Gearmulator MD or MM and open
   **Performance diagnostics**. Knobs may have their own parameter menu.
2. Click **Start performance capture**. Reopen the submenu to check its status.
3. Reproduce the crackling or slow playback using your normal host settings.
4. Click **Stop performance capture**, then **Open logs folder**.
5. Share the `performance-<timestamp>-<unique-id>.jsonl` file and whether you heard
   the problem. Panel actions are recorded automatically. If there were several
   incidents, approximate audible-problem times still help identify which one.

The folder is `logs/` beside the product's `roms/` and `config/` folders, beneath
its public data directory. This also respects `GEARMULATOR_DATA_ROOT`.
Each plugin instance and each capture gets a separate filename. Existing reports
are preserved. Capture automatically stops after ten minutes or 8 MiB. Start a
new capture if you need another recording. The submenu displays file/directory errors
and automatic stops. The worker flushes approximately every two seconds, so a
crash can lose the most recent interval. Stopping or closing the plugin flushes
the report; a callback still in flight at stop may be absent from the final data.

Capture is session-only: reopening the plugin does not restore the capture setting.
For hosts without an open editor, set `GEARMULATOR_RT_INSTRUMENTATION=1` (also
accepts `true`/`TRUE`) **before starting the host**. MD/MM then automatically starts
an exported capture for each instance. Remove the environment variable to prevent
this on later launches. The context menu can stop an environment-started capture.

## Contents and interpretation

The file is JSON Lines: every complete line is independently parseable JSON.
Schema 2 contains a `session` header, cumulative `summary` records, selected
`callback` records, panel/transport `event` records, and an `end` record (`stopped`
or `capture_limit`). Older schema-1 reports have no action timeline. Durations
are nanoseconds. Summary elapsed time and callback start time are relative to
capture start; they use a monotonic clock.

The session identifies the product, version/build revision, host/plugin format,
OS, CPU model/vendor, logical CPU count, reported clock speed and process
architecture. Callback records include actual block size and sample rate,
device sample rate, resampler mode, DSP clock percentage, active output layout,
transport/bypass/offline state, and incoming MIDI event/byte counts. The action
timeline records panel button states and encoder movements. No MIDI payload,
audio, presets, project contents, or firmware data are recorded. Panel actions
can reveal which trigger keys a person pressed.

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
Timings measure elapsed wall time, which can include OS preemption and waits;
device-dominated time alone does not prove continuous CPU execution in emulation.

## Panel and transport timeline

Event `timeNanoseconds` uses the same capture-relative monotonic clock as callback
`startNanoseconds`. Sort by timestamp when correlating actions and slow callbacks:
the writer drains separate queues, so JSONL line order is not chronological.
`sequence` identifies an event recording attempt; concurrent producers can enqueue
attempts out of order.

Panel events include the raw `model`, `command`, and `argument`, plus readable
labels. Button packets describe a complete row state: `buttonsDown` and `buttonsUp`
list controls separated by `|`. They are not individual button-edge claims.
Encoder packets include `encoder` and signed `steps` (a decimal string).

| Panel phase | Meaning |
| --- | --- |
| `submitted` | The editor is about to send a panel packet, before acquiring the synth lock. |
| `result` | The send returned; the same `inputId` pairs it with submission. `accepted` is the device queue API result. A false result can involve queue overflow/recovery, not only a missing device. |
| `delivered` | An audio callback handed the packet to the emulated MCU's UART2 RX queue. `callbackIndex` and `deferred` identify that callback and live/replacement machine. This does not establish when firmware processed it. |

Delivery has `inputId:0`: the functional panel queue is unchanged. Recovery,
coalescing and inputs from other sources prevent a guaranteed one-to-one match
with editor submissions. Match packet values and timestamps with that limitation;
do not infer exact per-input firmware latency from an ambiguous match.

The common editor send path covers panel presses, releases, modifier chords,
latches, generated releases and encoder detents. It records inputs, not confirmed
firmware actions or a full replay. Untouched buttons held before capture are not
reconstructed. Preset/settings operations and LCD/LED changes are not a separate
semantic timeline.

`host_transport` records the first observed state (`initial:true`) and subsequent
changes to playing, offline, bypassed or transport availability. With `known:false`,
the host did not provide transport information; ignore `playing`. Internal panel
Play and the host's Play state are distinct. Host seek/loop positions, tempo and
automation values are not recorded. There is no automatic audible-glitch marker.

A separate preallocated 1024-entry queue serves panel and transport events. Each
producer makes one nonblocking attempt; contention or a full queue drops only the
diagnostic event. Summaries expose `timelineEvents` (attempts) and
`timelineEventsDropped`. Slow-callback volume cannot fill the action queue. Capture
limits, stopping and event drops can truncate the timeline, so absence of an event
does not prove an action never occurred.

## Implementation and validation

The audio path records into preallocated storage, with no report formatting,
filesystem calls, worker notification, or waiting for queue capacity. A dedicated
`PerformanceReport` worker is the sole trace consumer and handles control labels, formatting,
file I/O, periodic flushes and limits. The processor owns the writer and destroys
it before destroying the synth. Capture stops on open/write/flush failures.

`synthLibPerformanceReportTest` covers correlated phase/context capture, offline
accounting, queue overflow and concurrent draining, actual synth processing at
44.1/48/96 kHz, JSON escaping, output, file errors, automatic limits and rapid
start/stop. Timeline tests cover disabled capture, input/result pairing, callback
delivery, initial and changing host state, session boundaries, bounded overflow,
immediate return under contention, concurrent producers/draining, and off-thread
label formatting. `synthLibAudioInstrumentationTest` runs the prepared-audio
allocation regression with capture scopes, transport and delivery events enabled.
These are included in the focused CI gate.
