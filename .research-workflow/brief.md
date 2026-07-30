# Research or competition brief

## Mode and objective

- Mode: competition
- Track/domain: arm-aarch64-compiler
- Research question or competition objective: Improve legal AArch64/Cortex-A53 execution of structurally matched triangular dynamic programs.
- Falsifiable hypotheses or ranking criteria: Prefer transformations that remove O(B^3) cache-unfriendly work; reject wrong, flat, regressed, or non-compliant candidates.
- Primary deliverable: One independently validated optimization candidate on zct-arm-no-magic.

## Authority and timeline

- Official source:
- Track:
- Registration deadline:
- Submission deadline:
- Submission quota:

## Evaluation contract

- Primary metric: Official Cortex-A53 runtime with correctness.
- Tie-breaking:
- Hidden/public evaluation behavior:
- Local evaluator command:

## Constraints

- Hardware/runtime/memory: AArch64/Cortex-A53; candidate shadow limited to 16 MiB.
- Allowed data/models/libraries:
- Network and packaging:
- Licenses/disclosure:

## Deliverables

- Required artifacts: Source, structural tests, validation logs, experiment record, commit, and pushed delivery revision.
- Rebuild/rerun procedure: Build and test inside compiler-impl2026-dev with /workspace/build/compiler.

## Baseline

- Source: commit 063502f, accepted content 233b204.
- Validation strategy: staged-expensive
- Baseline status: attested
- Reproduction or verification command: Disable candidate switch for exact baseline comparator; full repository ARM gates before publishing.
- Expected result: 60/60 performance and, for this shared memory transformation, 140/140 correctness.

## Unresolved rules

- [ ]

## Acceptance gates

- [x] Metric reproduced locally
- [x] Baseline confidence matches claim/submission risk
- [x] Correctness/leakage checks pass
- [x] Winning changes have controlled evidence
- [x] Submission dry-run passes
