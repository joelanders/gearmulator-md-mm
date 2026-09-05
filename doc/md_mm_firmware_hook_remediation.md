# MD/MM firmware-hook remediation

Prepared 2026-09-05 on `refactor/md-mm-firmware-hooks`, based on merged release
commit `65fe402deb87b60e279378f164aef620d33e672f`. The DSP submodule starts at
`363d3fc0632392a4cc9329cf5fd6e9f53e7a8ff6`.

Status: investigation and implementation plan. **No runtime hook has been
removed or replaced by this preparation commit.** The existing MD/MM behavior
is the comparison baseline, not proof that the hooks accurately model hardware.
This document does not assess legal permissibility or establish clean-room
provenance. Moving code into another module does not change how it was derived.

## Objective and scope

Replace host-side knowledge of firmware program locations, private data
structures, and algorithms with justified hardware or external-protocol
behavior. Keep each removal separately reviewable and record its evidence.

Initial scope is MD/MM and its DSP core integration. The inherited Nord Lead 2x
master-tune implementation and its analysis document are separate findings;
changing another synth's behavior should be a separate work item. Ordinary
memory maps, instruction implementations, MIDI messages, and panel protocols
are not automatically candidates for removal merely because constants appear
in their implementations.

## Findings and replacement work

| Case | Current location | Replacement direction | What remains unresolved |
| --- | --- | --- | --- |
| MM sample-buffer correction | DSP `dsp.cpp`, `dspMmCleanGndSinStep`; enabled by MD/MM `mddsp.cpp` | Reproduce the affected nominal-rate audio through correct CPU/peripheral/serial behavior, then remove the firmware-PC hook and its dispatcher special cases. | Underlying discrepancy has not been isolated. Need a reproducible MM audio fixture and reference observations before choosing a fix. |
| Panel-ready task-list updates | `mdmc.cpp`, `panelDisplayReadyPost` and its periodic caller | Have the emulated panel/peripheral signal readiness through the proper external interface, allowing firmware to update its own task lists. | The correct readiness signal and timing must be established; do not assume that sending an arbitrary UART byte replaces the semaphore update. |
| MM parameter-transfer ordering | `mddsp.cpp`, firmware-handle check in `writeWordToDsp` | Express transfer readiness through HI08 buffering, status, interrupts, and scheduling rather than inspecting a firmware variable. | Need to reproduce the parameter-transfer failure with the hook disabled and determine which hardware handshake or scheduling property is missing. |
| Factory DigiPRO waveform injection | `mdmmwaveforms.h`, called by `Hardware` construction | Investigate whether normal firmware execution should populate DSP memory through an emulated transfer path; implement that path if missing. | Fixed source/destination layout is currently assumed. Establish the actual transfer/storage behavior and compare every slot, including the four spill words. Bytes currently come from the supplied ROM, not an embedded waveform bank. |
| Synthetic DSP boot response | `mddsp.cpp`, boot-query interception | Establish which component provides the response on hardware and model that component or transfer sequence. | Removing the response may stall boot. Its origin and role need evidence before changing it. |
| Panel startup handshake | `mdmc.cpp`, `onPanelTransmit` | Encapsulate the absent panel controller as an external-protocol device with explicit reset/startup states. | Establish provenance of the protocol description. This may be appropriate protocol emulation already; it should not be conflated with direct task-list rewriting. |

The panel and boot code's comments attribute behavior to MAME. Those comments
are leads for a source/provenance check, not independent verification that the
current implementation matches a particular public driver revision.

## Preparation that is clear

- Establish a baseline for each affected behavior and make hook activation
  observable in diagnostic builds. Keep diagnostics out of normal product UI.
- Separate panel-controller responsibilities from MCU execution, and hardware
  transport responsibilities from firmware-specific compatibility behavior.
- If temporary diagnostic switches are needed, make them explicit and local
  to this investigation; a switch that leaves the workaround enabled is not
  remediation completion.
- Preserve ROM fingerprint validation. Normal MD/MM loading already accepts
  only pinned supported images; the CPU hook lacks a separate check but is
  reached through that validated loader.
- Keep the recent MERGE, flag-preservation, DMA-wrapping, codec-routing, and
  mode-label fixes. Their correctness does not depend on retaining these hooks.

## Order and acceptance criteria

1. **Establish MM reproduction coverage.** Identify the supported MM fixture,
   capture the affected sine/nominal-rate case, DigiPRO output, and parameter
   changes. Confirm the current behavior first. Retain the existing MD boot,
   mode, RAM audio, and randomized scheduler tests as guards.
2. **Panel readiness and startup.** Validate startup and sustained LCD/LED
   updates using external interface events, with direct firmware task-list
   writes absent. Cover both models, first-run initialization, reset, and state
   restoration.
3. **Host transport and boot responses.** Check event ordering and progress
   without the parameter-memory hook or fabricated query response. Cover boot
   plus rapid parameter changes across voices, with bounded queues and no
   firmware-PC/private-variable conditions in the replacement.
4. **Waveform population.** Demonstrate DigiPRO bank availability and correct
   playback through the established hardware path without constructor injection.
5. **Audio correction.** Remove the CPU hook once nominal-rate and deliberately
   reduced-rate output are explained and validated. Check interpreter/JIT
   parity and bounded execution on ARM64/x86-64.

Each step should identify the relevant public hardware/protocol reference or
the permitted source of observations, add the narrowest useful regression,
show the replacement working with the old hook absent, and remove its obsolete
state and special cases. Do not replace one firmware address or task-layout
dependency with a different one.

The preferred implementation strategy is incremental behavior preservation.
An immediate removal strategy is materially different: it may disable boot,
panel operation, DigiPRO audio, or parameter updates. Select that tradeoff
explicitly before making dependent runtime changes.

## History and review boundaries

Most MD/MM cases were already present in the August 26 integration commit
`bd5800b8` (165 files, 14,477 insertions). Its DSP pointer was `329f5977`, which
already included the MM audio correction; corresponding later DSP history
contains `00c48833`. Waveform transfer behavior was refined in `635724ca` on
September 2. These hooks were not introduced by the recently merged RAM-audio
PRs #5/#42.

Repository history identifies integration points, not which person or model
originally devised a technique or what review/authorization occurred elsewhere.
The recent RAM-audio review covered its changed behavior; it was not a complete
firmware-provenance review of the pre-existing emulator.

Related notes:

- [RAM audio validation](md_ram_audio_validation.md)
- [RAM audio fix provenance and upstream candidates](md_ram_audio_fix_provenance.md)
