# Research contract

## Contract identity

- Version: v1
- Status: frozen candidate screen
- Frozen at: 2026-07-30
- Selected idea ID: arm-triangular-transpose-shadow-v1

## Claim and mechanism

- Falsifiable hypothesis: For structurally proven descending triangular dynamic programs, an internal transposed shadow of finalized cells changes the hot second matrix load from a row-stride stream to a contiguous stream and yields a meaningful official Cortex-A53 improvement without changing output.
- Baseline: commit 063502f on zct-arm-no-magic; accepted optimization content equals 233b204.
- Intervention or controlled change: Add one AArch64-only mid-end pass after loop scalar promotion; no other optimization changes.
- Proposed mechanism: Preserve every original matrix write, mirror each finalized upper-triangular cell as shadow[column][row], and read the finalized dependency through the contiguous shadow row.
- Scope and exclusions: Rank-two i32 objects with a constant semantic bound, descending outer IV, ascending middle/inner IVs, exact triangular dependence, no calls or unknown matrix writes in the outer-loop region, at most 16 MiB shadow, and one candidate per module. No names, paths, benchmark constants, source fingerprints, or undefined behavior are inputs.

## Evaluation design

- Primary metric or observation: Official Cortex-A53 runtime for the affected ARM performance workload.
- Secondary metrics: Hot-loop assembly stride, local QEMU A/B timing, code size, 60 performance correctness, and 140 correctness cases.
- Data split, workload, or target conditions: Official ARM/AArch64 track, -O1, Cortex-A53 acceptance target.
- Controls and comparators: Same source and toolchain with COMPILER_DISABLE_TRIANGULAR_TRANSPOSE_SHADOW=1 versus enabled candidate.
- Repetition, seeds, folds, or trials: Three local timing repetitions for screening; repository owner official submission decides acceptance.

## Decision signals

- Success signal: 60/60 performance and 140/140 correctness, structural synthetic suite passes, and a meaningful repeatable official runtime improvement.
- Failure signal stated independently: Any wrong result, verifier/build failure, rule violation, material resource failure, isolated official regression, or statistically flat official result.
- Inconclusive region: Local QEMU-only improvement or noisy official movement without a stable meaningful gain.
- Mandatory ablations: Disable switch restores baseline IR/assembly; renamed and resized structural positive; dynamic-bound, altered-IV, alias/effect, and extra-store fallbacks.

## Verification ladder

- [x] L0: paper consistency, dimensions, assumptions, complexity
- [x] L1: hand-checkable, synthetic, or tiny-case sanity check
- [x] L2: minimum-cost falsification run
- [x] L3: full-scale controlled experiment
- [x] L4: ablation, robustness, variance, transfer, and resource checks
- [ ] L5: clean-environment or independent reproduction

## Budget and stopping

- Compute/time budget: Minimum structural tests and h-8 A/B first; full 60/140 only if those pass.
- Stop conditions: Stop before full gates if the pass does not match structurally, does not produce contiguous access, fails correctness, or loses materially in minimum-cost timing.
- Safety, ethics, rules, and licensing constraints: Repository Competition Compliance Rules; independent implementation; AI-assisted matching, transformation, tests, and documentation must be disclosed.

## Amendment history

- None. Create a new version rather than silently changing frozen criteria.
