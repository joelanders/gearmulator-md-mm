# MD RAM audio fixes: provenance and possible upstream contributions

Recorded on 2026-09-05, after review of
[`dsp56300-md-mm` PR #5](https://github.com/joelanders/dsp56300-md-mm/pull/5)
and [`gearmulator-md-mm` PR #42](https://github.com/joelanders/gearmulator-md-mm/pull/42).
For symptoms, technical explanations, validation, and remaining hardware risks,
see [MD RAM audio validation](md_ram_audio_validation.md).

These PRs fix a mixture of inherited DSP emulator gaps and bugs introduced by
our MD/MM fork. They do not modify Elektron's firmware. This distinction matters
if we later prepare contributions to the upstream DSP emulator project.

## Scope of the provenance check

The evidence below comes from local Git history, including blame on the code
before these fixes and inspection of the relevant commit diffs. It establishes
what our fork inherited and subsequently changed; it does **not** establish
whether the latest upstream branch still has each issue. Recheck upstream before
opening a PR. Blame dates identify the traced historical lines, not necessarily
the first-ever introduction of a bug.

Reviewed implementation heads:

- DSP repository: `363d3fc0632392a4cc9329cf5fd6e9f53e7a8ff6`.
- Main repository: `b59bf4b31a60a48cf8db6e5c80455ccede4b8a16`.

## Findings

| Change | Provenance | Evidence and upstream relevance |
| --- | --- | --- |
| Implement `MERGE` in interpreter and JIT | Inherited upstream omission | The interpreter stub in `dsp_ops_alu.inl` traces to `17dda2d8` (2021-04-23); the JIT stub in `jitops.h` traces to `2473575f` (2021-05-19). A general instruction implementation is a candidate for upstream. |
| Declare `MERGE` destination as read/write | Inherited upstream metadata error | `opcodeanalysis.h` marked the destination write-only in `8aa104cc` (2022-01-09), although the operation consumes its previous contents. Include this correction with the instruction implementation. |
| Preserve pending E/U flags before `MERGE` updates N/Z/V | Bug in our initial implementation | `ac2351a4` used `resetCCRCache()`, discarding deferred flags from preceding arithmetic. `930447ef` replaces it with `updateDirtyCCR()` and adds sequence coverage. Upstream should receive the corrected implementation, not the intermediate version. |
| Restore 24-bit 3D DMA address wrapping | Regression introduced by our fork | The mask was present in historical commit `5f1568d8` (2026-03-26 author date). Our scheduling/peripheral commit `00c48833` removed `_target &= 0xffffff;`. `ac2351a4` restores it. Do not present this restoration as a newly discovered missing upstream fix. |
| Correct Classic/Extended labels | Our MD/MM integration | The reversed enum values in `mdfrontpanel.h` trace to our MD/MM support commit `bd5800b8` (2026-08-26). This is specific to our front-panel model. |
| Deliver codec input to MD DSP2 | Our MD/MM hardware model | DSP2's ESSI1 silence callback traces to `bd5800b8`. The single-receiver input assumption is also explicit in `6dcbdcb8` (2026-08-31). Independent queues correct our model of the shared ADC bus. |
| Suppress empty MD DSP2 ESSI0 receive edges | Our MD/MM integration | The callback wiring is in our `mddsp.cpp`. It applies the existing availability mechanism to the MD main-mix link; it is separate from codec input delivery over ESSI1. |
| Improve telemetry and firmware/audio tests | Our validation infrastructure | Per-DSP counters, signal-fidelity checks, and named firmware test functions improve our fork's diagnostics and regression coverage. They are not fixes to Elektron's firmware. |

## Suggested upstream contribution

Prepare a small, self-contained **MERGE implementation and regression tests** PR
against the then-current upstream base, if still needed:

1. Implement the normal 24-bit packing operation in interpreter and JIT.
2. Preserve the other accumulator fields and unaffected condition flags,
   including flags still pending from preceding arithmetic.
3. Correct the destination dependency in opcode analysis.
4. Include firmware-independent tests for all six source registers, both
   destinations, source/destination aliasing, distinct half words, N/Z/V, and
   preserved flags after an ADD/MERGE sequence.

Reference implementation: DSP commits `ac2351a4` and `930447ef`. Extract the
relevant changes rather than blindly cherry-picking `ac2351a4`, which also
contains our DMA-mask restoration. Check the upstream flag-cache, register
representation, and JIT APIs when adapting the patch.

The instruction reference is the DSP56300 Family Manual, Rev. 5, MERGE entry
(13-108). **Sixteen-bit Arithmetic mode is distinct from S0/S1 scaling modes.**
Our implementation covers the normal 24-bit operation used by the traced MD
firmware; it does not implement Sixteen-bit Arithmetic mode. Explain that scope
explicitly and follow the upstream project's expectations for unsupported modes.

The DSP suites passed on native ARM64 and x86-64 under Rosetta. A negative
control restoring our old cache reset failed the new flag-preservation test.
The real MD OS 1.63 regression provides additional integration evidence, but
the upstream PR's tests should not require proprietary firmware.

## DMA contribution should remain separate

DSP commit `363d3fc0` adds single-word-request regressions for 24-bit source
address wrapping: in-line increment and positive/negative DOR-A and DOR-B
offsets. Removing the mask made those tests fail.

Those tests may be useful upstream after adaptation, even if its mask is already
correct. First inspect its current DMA implementation and counter/trigger
semantics. Do not bundle our broader MD/MM scheduling changes into a MERGE PR,
and do not claim these tests exhaustively validate DMA: the current boundary
cases exercise the 3D source side, not every source/destination mode.

## Local commits for continuation

- DSP initial implementation and DMA restoration: `ac2351a4`.
- DSP flag preservation, operand coverage, and dependency correction: `930447ef`.
- DSP DMA boundary regressions: `363d3fc0`.
- Main mode labels and MD link change: `654fdf53`.
- Main independent codec-input receivers: `069c900b`.
- Main per-DSP telemetry and updated DSP submodule: `540787e1`.
- Main waveform-fidelity tests, test organization, and validation note: `b59bf4b3`.

No upstream PR was opened as part of this work. The two linked PRs target our
forks; an upstream submission is future work.
