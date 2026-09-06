# EFM-SD and DSP accumulator correctness

This update fixes an x86 DSP JIT defect that corrupts EFM-SD oscillator phase. The reported settings are PTCH=64, DEC=96, NOISE=0, NDEC=64, MFRQ=64, MDEC=127, HPF=0, velocity=110. MOD=0 loses the sustained tone; MOD=32 produces broad noise instead of the hardware-like tone.

The dependency update also fixes related accumulator load, shift, rotate, saturation and boundary-flag errors found by a targeted audit. Some corrections apply to ARM and to the interpreter as well as x86. It incorporates the latest released DSP changes and four focused fixes extracted from the firmware-hook work, without adopting that broader draft.

The new regression target also runs in the plugin repository’s existing Linux Release, macOS and Windows checks.

The DSP companion is [PR #8](https://github.com/joelanders/dsp56300-md-mm/pull/8). Its permanent tests include 50 explicit expected-value cases and 7,680 deterministic interpreter/JIT comparisons per architecture, requiring no private firmware or tester recordings. The original bounded audit has no remaining mismatches in the corrected local ARM64 and x86-64 runs; this is not an exhaustive instruction-set correctness claim.

The original failure was reproduced on physical Intel macOS as well as x86 execution under Rosetta. With the corrected DSP, the known-working headless MD setup produces identical native ARM/x86 samples for the GND-SIN control and both reported EFM cases; at a 48 kHz host rate, the maximum difference is below 4.5e-8. Cross-platform CI and validation with the latest host source are recorded in the PR before it is marked ready.

No GUI setting, firmware change, profiling capture or user migration is required. Existing installed plugins are unaffected until a build containing the updated dependency is installed.

The confidence follow-up adds 54 explicit expected-result cases and 464 multi-instruction cases alongside the 7,680 single-instruction comparisons. It also corrects stale interpreter negative flags after logical shifts. Negative controls reject the alpha.10 DSP core. The integration includes alpha.10's latest project-restore tests; the additional regression steps run in Linux, Windows and macOS plugin CI.

With a freshly rebuilt MCU, all 61 Machinedrum and 15 representative Monomachine native ARM before/after audio captures are byte-identical. Local Device state restore/save checks on ARM and x86 require unchanged parameter readback and audible before/after output for EFM-SD and FM+STAT, including MM user flash. These headless checks supplement the built-VST3 CI gate; they are not an Ableton play test or proof of hardware waveform parity. Full cross-architecture audio and performance receipts are recorded in the PR.
