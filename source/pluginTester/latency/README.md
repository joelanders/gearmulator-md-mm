# Packaged-plugin latency measurements

This JUCE host loads an explicit VST3 (or AU on macOS), supplies timestamped MIDI/playhead/input, and saves stereo audio plus every callback's duration. It does not install plugins or open an audio interface. Use the same host, firmware, seed, settings and machine load for both sides of a comparison.

## Build and test

From a recursive checkout, with CMake, a C++17 compiler and JUCE's platform dependencies installed:

```sh
cmake -S source/pluginTester/latency -B /tmp/latency-host -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/latency-host --config Release --parallel 3
python3 -m venv /tmp/latency-python
/tmp/latency-python/bin/python -m pip install numpy scipy
/tmp/latency-python/bin/python source/pluginTester/latency/test_analysis.py
```

The executable is `latency_host` in that build directory (Windows: `Release/latency_host.exe`; use the venv's `Scripts/python.exe`). Linux needs an X display, such as `xvfb-run`, for JUCE's message loop. The existing audio-I/O CI builds this host and the transport fixture tool through its JUCE layout target. Run the Python analysis oracles with the command above.

## Capture

Supply your own MD 1.63 / MM 1.32b firmware and an already-built plugin. Each output directory must be new. `--seed` optionally copies a product folder's `config/` and `nvram/`; it never reads Documents implicitly. Without a seed the plugin initializes fresh state. For patch-matched comparisons, copy a product folder once and reuse that snapshot. `--rom` always supplies the sole ROM in the isolated directory.

```sh
/tmp/latency-python/bin/python source/pluginTester/latency/run.py /tmp/md-before \
  --host /tmp/latency-host/latency_host --model MD \
  --plugin '/path/to/baseline/Gearmulator MD.vst3' \
  --rom '/path/to/elektron_sps1-1uw_os1.63.bin' --seed '/path/to/MD-seed' \
  --rate 48000 --block 128 --phase 127 --scenario notes
```

Repeat into `/tmp/md-after` with the candidate plugin. Repeat with `--model MM`, the MM ROM/seed/plugin, then use `--scenario input --rate 44100 --block 512 --phase 0` for each device. Input setup selects MD INP-GA or MM track-one FX THRU using SysEx/CC. The host saves the actual deterministic input probe. New instances use the legacy resampler and `--latency-blocks 0`; alternate resampler timing is covered by `synthLibResamplerTimingTest`.

Notes begin at 10 seconds, repeat every 3.137 seconds, and use velocity 100 with 150 ms gates; phase is relative to the requested fixed block. `--variable` varies actual callbacks. `--scenario chords`, `--reprepare SECONDS`, `--restore SECONDS`, `--offline` and `--suppress-message-loop` provide separate stress/diagnostic cases. The last two change controller dispatch behavior and must be labeled separately from normal paced runs.

## Recreate the MM transport fixture

Using the parent project's configured build, build the optional fixture tool. Give it a **copy** of a firmware-initialized MM patch-RAM image, normally named `nvram/mm-factory-live3-be.bin` in the product data folder. The tool reads that copy and writes only its new output directory.

```sh
cmake --build /path/to/project-build --config Release --target mdLatencyFixture
/path/to/project-build/source/elektron/md/mdLibTest/mdLatencyFixture \
  '/path/to/elektron_sfx6-60_os1.32b.bin' '/path/to/MM-seed/nvram/mm-factory-live3-be.bin' \
  /tmp/mm-transport-fixture
```

It enables external sync/transport and channel 1, clears a 16-step pattern, places track-one triggers on 1/5/9/13, selects GND-SIN, shortens its envelope and removes delay. SysEx readback verifies configuration; a final clock-driven render must produce audio. Dumps, patch RAM and the diagnostic render remain private.

Run `run.py` with `--model MM --scenario transport --patch-ram /tmp/mm-transport-fixture/mm-factory-live3-be.bin`. The playhead starts at 10 seconds/120 BPM, changes to 180 BPM at 14 seconds, loops to PPQ zero at 16.173 seconds, and stops two seconds before the end. Actual block-aligned transition samples are recorded.

## Inspect and compare

`run.py` invokes `analyze.py`; rerun it as `python source/pluginTester/latency/analyze.py CASE_DIRECTORY`. Keep `capture.wav`, input WAV, callback CSV, metadata and `run.json` together. The analyzer verifies capture hashes and timeline continuity. `summary.json` removes local paths and contains numeric evidence plus hashes; review it before sharing. ROMs, NVRAM, logs, audio and profiles belong outside Git.

Compare note onset at several thresholds, requiring a quiet pre-window. Input correlation reports signed delay in four windows and returns null for absent/uncorrelated audio. Transport analysis uses three thresholds because long tails can merge pulses. Callback results separate the first-note 250 ms window from warm samples at 12 seconds onward; compare p50/p99 and counts exceeding each actual block period. Quantized WAVs are preceded by a finite-output check in the host.

Acoustic onset depends on the instrument, envelope and routing. Silence or a nonquiet pre-window is not a passing latency result. Paced callback overruns describe this host/OS workload; they do not establish Ableton PDC or physical audio-interface latency. Instrumented optimization-training runs are not benchmarks, and new optimized candidates require source-matched profiles.
