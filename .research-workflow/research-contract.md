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

- 2026-07-30 safety amendment: official Cortex-A53 confirmed the mechanism, but
  a middle-loop early-exit counterexample falsified the original must-execute
  assumption. The implementation now requires a pure outer header, an
  unconditional outer-body edge into the middle header, a unique normal middle
  exit, and no non-latch edge back to the outer header. These checks only narrow
  eligibility and leave the accepted h-8 assembly unchanged.

## Contract v2: crypto modular state-machine simplification

- Frozen at: 2026-08-03
- Selected idea ID: arm-crypto-modular-state-machine-v1
- Hypothesis: a general i32 modular-affine canonicalizer, followed only when
  structurally proven by immutable local-array value forwarding and bounded
  pure-loop expansion, removes a material fraction of the serial crypto hot
  path without using benchmark identity or changing wrapping semantics.
- Baseline: commit `3a921cf` on `zct-arm-no-magic`.
- Primary observation: official Cortex-A53 time for the affected programs.
- Controlled local evidence: enabled versus independent component switches on
  crypto-1/2/3, final assembly, QEMU timer, all 60 performance cases, and 140
  correctness cases because this is shared mid-end code.
- Success: byte-identical outputs, structural anonymous tests and fallbacks,
  no demonstrated target-relevant regression in other changed programs, and a
  clear local hot-path reduction sufficient to justify official submission.
- Failure: wrong output, incomplete overflow/alias/effect proof, code growth
  without hot-path reduction, focused timing regression, or a full-gate failure.
- Mandatory ablations: disable affine canonicalization; disable bounded
  recurrence specialization; disable the complete candidate.
- Budget and stop rule: run the affine stage first and kill it if crypto final
  assembly does not simplify; run full 60/140 only after the integrated focused
  candidate is correct and locally positive.
- Compliance: triggers may use only IR/MIR semantics, CFG, dominance, use-def,
  loop/range, alias, escape and effect analysis. Names, paths, labels, source
  text, public inputs, constants used as identities, and runtime probing are
  prohibited.
- Local outcome: both components passed their anonymous differential suites.
  The integrated candidate changed only crypto-1/2/3 among the 60 final
  assemblies, reducing each from 655 to 601 parsed instructions. The focused
  timer sum was `0.181242s` with both components disabled, `0.152859s` with
  affine canonicalization alone, and `0.138165s` with both enabled. Full gates
  were `57 AC + 3` pre-existing h-4 formatting exceptions and `140/140 AC`.
- Promotion state: locally validated and pushed only for official Cortex-A53
  evaluation; no official speedup is claimed until the owner reports it.

## Contract v3: loop-invariant guard-chain hoisting

- Frozen at: 2026-08-03
- Selected idea ID: arm-loop-invariant-guard-hoisting-v1
- Hypothesis: when a counted inner loop begins with a short-circuit guard chain
  whose leading predicates are invariant for the whole loop, evaluating those
  predicates once in the unique preheader and skipping the effect-free loop
  when they fail reduces Cortex-A53 control-flow work without changing results.
- Baseline: commit `d9db8a8` on `zct-arm-no-magic`.
- Preconditions: unique preheader/backedge/exit; zero-based unit-step signed
  induction with a loop-invariant bound; no loop-defined SSA value escaping;
  no exit Phi; cloned predicates contain only non-trapping pure integer work or
  loads from non-escaping, loop-unmodified scalar allocas; skipped prefix work
  has no observable stores, calls, division or remainder. Both true/false CFG
  orientations are normalized. Dynamic bounds and zero-trip loops are kept.
- Primary observation: official Cortex-A53 time for structurally affected
  programs. Local QEMU is a submission screen only.
- Controls: `COMPILER_DISABLE_LOOP_INVARIANT_GUARD_HOISTING=1`, O0 output,
  renamed/resized positive cases, reversed branch orientation, zero-trip and
  invariant-false boundaries, varying-guard fallback, observable-prefix
  fallback, all 60 performance cases and all 140 correctness cases.
- Success: structural differential output equality, complete gates, a reduced
  hot-loop branch count and a meaningful official target improvement.
- Failure: any skipped observable effect, induction/termination proof gap,
  output mismatch, full-gate failure, or flat/regressed official result.
- Local outcome: anonymous enabled/disabled/O0 outputs agree and two differently
  oriented guard chains transform; the observable-prefix and varying-guard
  cases fall back. Final performance is `57 AC + 3` unchanged h-4 formatting
  exceptions, correctness is `140/140 AC`, and only conv2d-1/2/3 assembly
  changes. Their single final QEMU screen is `0.071782s -> 0.061491s`, about
  `14.3%` lower.
- Compliance: the pass consumes only IR opcode/operand/type, CFG, dominance,
  use-def, loop induction and non-escape/effect facts. It does not inspect any
  path, source text, symbol/function/block name spelling, benchmark constant,
  generated label text, input or output.
