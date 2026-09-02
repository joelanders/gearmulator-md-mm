# MD/MM Windows audio diagnostic build

This build measures where each audio callback spends its time. It does not
change the emulator scheduler or locking behavior.

## Running the test

1. Download and unzip the Windows diagnostic artifact. Firmware is not included;
   install the MD or MM firmware exactly as for a normal build.
2. Launch the standalone application. A diagnostic TSV file is created on the
   Windows desktop. Its name begins with `Gearmulator MD Audio Diagnostics` or
   `Gearmulator MM Audio Diagnostics`.
3. In Audio Settings, first select the same WASAPI exclusive configuration that
   showed the problem. Note the device name, driver type, sample rate, and
   requested buffer size.
4. Leave transport stopped for at least 10 seconds. Press play and let it run for
   at least 30 seconds while the stutter is audible. Stop it and wait another 10
   seconds.
5. Repeat with the 8192-sample buffer. If an installed ASIO driver now appears,
   repeat once with ASIO using the nearest available sample rate and buffer size.
6. Close the application so the final row is written. Send the TSV file together
   with the device/driver notes, CPU model, and whether the CPU percentage came
   from Gearmulator, Windows Task Manager, or another program.

The file contains no firmware, audio, usernames, project data, or file paths. It
records the operating-system version, CPU model, source commit, actual callback
block sizes, callback load and deadline misses, mutex wait time, resampler and
emulator time, queue failures, and real-time allocation fallbacks.

## Reading the result

- High `lock_wait_total_ms` or `lock_wait_max_ms` means another thread is blocking
  the audio callback on the shared device mutex.
- High `emulator_total_ms` with little lock wait means the emulated processors and
  scheduler are the bottleneck.
- A large `resampler_overhead_ms` points to sample-rate conversion or channel
  processing outside the emulator.
- `deadline_misses` means callbacks are taking longer than the audio time they
  must produce.
- Any nonzero queue underflow or overflow counter identifies an audio-plumbing or
  scheduler discontinuity even when average callback load is below 100 percent.
