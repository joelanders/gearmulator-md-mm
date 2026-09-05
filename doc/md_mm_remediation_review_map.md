# MD/MM remediation: smaller review units

Prepared 2026-09-05 against DSP `8e4708b0`, MCU `62dc26cd`, and the main
`refactor/md-mm-firmware-hooks` integration branch. This is an extraction and
dependency map, not a claim that the extracted branches already exist or have
passed their own tests. Keep the original drafts as integration history; do
not force-push, merge or close them as part of preparing smaller reviews.

## Why the drafts grew

Main #43 combines hook removal, hardware integration, test infrastructure and
the investigation journal. DSP #6 combines the core audio-hook removal with
processor, interrupt, flag and DMA fixes discovered during validation. MCU #3
provides the host-side interface for part of that transport work. Most added
lines are tests/documentation, but shared-core changes still have broad risk.
There are a handful of firmware-dependency categories, not one firmware hack
per newly corrected processor bug. The old emulator is a regression baseline,
not an independent hardware oracle.

## Candidate review families

Each row can require multiple small PRs. Prefer one behavior correction with
its independent tests and short evidence note; do not transplant an entire
mixed commit merely to avoid separating its concerns.

| Family | DSP commits | Boundary and acceptance |
| --- | --- | --- |
| Arithmetic flags | `9223c5a2`, `4b4a2e22`, `c6d3a21c`, `9e3834c2`, `0e34f71f` | E sign, ASR, NEG, ASL, ADDL; separate public-ISA cases. Run each branch's full interpreter/ARM64/x86-64 core suites. |
| Deferred flags and result fields | `864a2910`, `22604267`, `3d5469ac`, `2dc9e867`, `67e7f765`, `412356e4` | CLR, CLB, partial CCR, runtime U, rotates and logical shifts. Preserve explicit/pending flag contracts; independently verify operand fields and untouched flags. |
| Conditional execution | `d6874c0e`, `8e4708b0` | Register pressure/exhaustion and compound truth tables are distinct corrections. Preserve pressure coverage and distinguish decoder correctness from instruction timing. |
| Cooperative loop execution | `50ff78f0`, `717dd7cb` | DO/DOR yielding and memory-sourced counts. Keep loop state, nested/interrupt behavior and scheduling evidence together; this has a wider execution-model risk. |
| HI08 transport | `5283572a`, `f7f45726`, `febeab64`, `aa346c11` plus MCU #3 | HCIE/source tagging, reset/cancellation, shared acceptance and command admission. Keep unresolved MM receive-word loss and pacing assumptions explicit. |
| DMA and diagnostic control | `608a6542`; separately `c078ec0b` | DMA done-status and loop-preserving recompilation are different review units. Recompilation controls are diagnostic infrastructure, not a firmware-level fix. |
| MD/MM hook removals and integration | `13295e64` plus main #43 | Remove the core MM sample correction and integrate the external-protocol replacements. Full firmware gates required; the private MD panel dependency is still unfinished. |

The previously merged MERGE and 24-bit DMA-wrap commits (`ac2351a4`,
`930447ef`, `363d3fc0`) are not new work in DSP #6. The local release branch
name can lag the PR base; use the actual target revision, not a branch-name
assumption, when extracting or counting changes.

## Known dependencies and ordering

- Keep the current chronological order within an extracted stack until the
  new base has been validated. Passing on the integration branch does not
  prove that an isolated cherry-pick passes on the upstream base.
- Rotate sequence tests rely on runtime-U correction `2dc9e867`. Later
  logical/rotate sequences also exercise the partial-CCR contract; do not
  discard those assertions to make a smaller branch green.
- `8e4708b0` extends the actual-dispatch conditional-transfer test introduced
  by `d6874c0e` and retains its flush-before-temporary-allocation rule.
- MCU #3 contains `01b89c8`, `aec3524`, `62dc26c`. The main hardware bridge
  needs compatible DSP and MCU APIs. Publish both submodule revisions before
  advancing the main pointer, and validate their combined behavior.
- After selecting an extraction base, check symbol/test dependencies, build
  all three backends, reproduce the pre-fix failure where feasible, run
  relevant firmware gates, and record the exact tested head. These steps
  have not yet been performed for new extraction branches.

## Existing PR overlap (checked 2026-09-05)

- [DSP #2](https://github.com/joelanders/dsp56300-md-mm/pull/2), head
  `f3383900`, changes `hdi08.cpp/.h` and shared test files for HCIE gating.
  It directly overlaps the transport family. The draft stack adds source
  tagging and acceptance/reset behavior beyond gating; compare exact semantics
  before deciding whether to reuse or supersede #2. Do not merge both blindly.
- [DSP #3](https://github.com/joelanders/dsp56300-md-mm/pull/3), head
  `90a38776`, changes `esai.cpp/.h` and shared test files. DSP #6 has no
  production-file overlap with that diff, but the common test files need
  careful integration. This file-level check is not a fresh functional review
  of the ESAI PR.
- Main #43, DSP #6 and MCU #3 remain open drafts. No PR was closed or merged
  while preparing this map. Other synth/UI/performance work remains outside
  this remediation scope.

## Work that cannot be presented as finished

MM receive-word loss, strict interpreter idle/audio, cap-1 playback quality,
the private MD panel task-list dependency, reset/state/edited-bank/panel
coverage and physical-device evidence remain open. Normal firmware tests and
synthetic conformance tests are complementary evidence, not hardware or legal
certification. After the current conformance fixes, functional investigation
must return to these symptoms, not expand into unrelated processor/synth work.

See [the complete acceptance checklist](md_mm_remediation_goal_checklist.md)
and [the chronological evidence journal](md_mm_firmware_hook_remediation.md).
