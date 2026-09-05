# MD/MM remediation goal: remaining scope and acceptance

This checklist records the user's expanded unattended-work scope, requested
after opening main PR #43, DSP PR #6, and MCU PR #3. It supplements the active
goal; it does not replace or narrow it. The goal API does not support editing
an active objective. Consult this checklist as well as
[the remediation journal](md_mm_firmware_hook_remediation.md) before claiming
completion. Passing a subset of tests is not completion of the full goal.

## Required remaining work

- [ ] Resolve MM reentrant host receive-word loss with an evidence-backed
  transport implementation. Establish receive-interrupt, directional INIT/reset,
  buffering and pacing behavior; eliminate unjustified compatibility assumptions
  incrementally, preserving MD and MM behavior. A fix enabled only for MD does
  not complete the MM requirement.
- [ ] Diagnose and fix interpreter MM idle-noise and sine-quality failures.
  Keep strict audio checks intact and validate interpreter/JIT behavior; a
  successful boot alone is insufficient.
- [ ] Establish independent evidence for panel startup/readiness behavior and
  replace the remaining MD private task-list manipulation. Preserve first-run
  UW initialization and sustained panel operation. Moving, hiding, disabling,
  or exchanging one firmware-internal dependency for another is not completion.
- [ ] Expand feasible regression coverage for reset and state restoration,
  edited waveform banks, sustained LCD/LED/panel behavior, parameter bursts,
  and audio. Run relevant ARM64/x86-64 JIT and interpreter gates, synthetic
  hardware/ISA regressions, and existing MD UW/RAM regressions.
- [ ] Establish available physical-device waveform/audio comparisons and
  independently justified protocol observations. If hardware, authorization,
  or documentation is unavailable, record precisely what evidence is missing
  and why; emulator-only tests do not establish hardware equivalence. Missing
  acceptance evidence must remain identified as incomplete, not checked off.
- [ ] Organize independently validated core fixes separately from unfinished
  compatibility work for review and potential upstream submissions. Assess
  shared-core regression risks without expanding into unrelated synth changes.
  Record dependency and merge ordering across main PR #43, DSP PR #6 and MCU
  PR #3, and identify overlap with existing PRs without closing them.
- [ ] Keep durable provenance, evidence, confidence, failed-experiment and
  limitations notes current. Commit/push justified changes and update the draft
  PRs so all in-progress work remains visible. Verify final branch/submodule
  state and audit every acceptance item against authoritative evidence.

## Original scope remains binding

Revalidate the implemented removals of synthetic DSP boot responses, the MM
firmware-PC audio correction, private parameter-memory guard, and factory
waveform injection as subsequent transport/core changes are made. Preserve ROM
fingerprint validation and the previously merged RAM, MERGE/flags, DMA-wrap,
codec-routing and mode-label fixes. Preserve user changes.

Use public hardware/protocol specifications, synthetic ISA programs and
authorized external observations. Do not derive replacements from firmware
disassembly, private addresses/layouts, or guessed internal algorithms. Do not
claim legal clearance or clean-room provenance from this technical work.

Work unattended where safe, documenting genuine external dependencies while
continuing other in-scope work. Do not merge PRs, close existing PRs, purchase
anything, or destructively rewrite history without additional authorization.

## Current starting point

The host execution stall is cleared for fresh UART tests: the register control
and new CPU-mask regression pass on x86-64, and the latter passes on ARM64.
This is not a remaining blocker. The other unchecked items above remain open;
see the journal for per-revision validation and known failed/reverted trials.
