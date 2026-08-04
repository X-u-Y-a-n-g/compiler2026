# Research or competition checkpoint

## Current objective

Publish the isolated AArch64 loop-invariant guard-chain hoisting candidate to
the owner-designated GitLab delivery branch, then wait for the official
Cortex-A53 result before starting the next independent crypto candidate.

## Confirmed facts

- Existing idempotent-iteration collapse already reduces conv2d's repeat loop
  to at most one iteration; remaining convolution work is scalar and repeats
  four boundary checks for every kernel tap.
- The new pass hoists the leading two row-bound predicates from the column-tap
  loop into its unique preheader. Column-dependent guards and border fallback
  remain unchanged.
- Anonymous O0/enabled/disabled tests cover zero trips, false invariant guards,
  dynamic dimensions, reversed branch orientation, varying guards and an
  observable-prefix negative case. Outputs agree; two positive loops transform.
- Final focused QEMU screen: conv2d-1/2/3 total `0.071782s -> 0.061491s`, about
  `14.3%` lower. QEMU is diagnostic only.
- Final gates: correctness `140/140 AC`; performance `57 AC + 3` parent-existing
  h-4 trailing-newline formatting exceptions. The three h-4 assemblies are
  unchanged. Only conv2d-1/2/3 final assemblies differ from the disabled build.

## Decisions and rationale

- Reject the earlier Phi remainder-sinking screen: it only removed one RNG
  fast-path remainder and showed no meaningful focused gain.
- Keep guard hoisting independent from interior-loop vectorization so official
  attribution is clear.
- Do not begin or publish the next crypto candidate until this pushed revision
  receives an official keep/revert decision.

## Next actions

1. Commit only the guard-hoisting implementation, tests and documentation.
2. Push `zct-arm-no-magic` to GitLab remote `eduxiji`.
3. Await official Cortex-A53 results; keep on meaningful improvement, otherwise
   revert and push before continuing crypto.

## Relevant artifacts

- `src/midend/pass/LoopInvariantGuardHoistingPass.cpp`
- `tests/optimization/loop_invariant_guard_hoisting.sy`
- `/tmp/eval-guard-final2`
- `/tmp/eval-guard-disabled`
- `/tmp/eval-guard-correctness-final`
