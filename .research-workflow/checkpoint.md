# Research or competition checkpoint

## Current objective

Publish the must-execute legality fix for the officially accelerated transposed-shadow candidate to the owner-designated AArch64 delivery branch.

## Confirmed facts

- Loop scalar promotion already removes the repeated destination load/store, but h-8 retains an O(B^3) row-stride dependency load.
- The final pass matches only a constant-bound descending triangular schedule, exact affine accesses, and a no-call/no-aliasing-store outer region.
- Anonymous 9x9 renamed/reformulated tests pass, including zero-trip inner loops, i32 wrapping values, and two sequential calls.
- A structurally similar 11x11 case with an extra matrix write falls back.
- The 60-case scan transforms only h-8-01/02/03.
- Final gates: correctness 140/140 AC; performance 57 AC plus the three already documented h-4 evaluator formatting exceptions. h-4 enabled and disabled assembly are byte-identical with SHA-256 1e7e46211863afd4643b72e689f5c9cfd93187b19bca82241f4f16d8384ba433.
- Official Cortex-A53 h-8 result is 3.55/3.55/3.54s versus the prior accepted 11.51/11.41/11.43s, a 69.0% aggregate reduction.
- A middle-loop early-break counterexample makes the old pass read stale shadow values and differ from O0; the legality fix rejects it.
- The fix adds compile-time-only must-execute/effect checks. Positive and h-8 final assembly remains byte-identical.

## Decisions and rationale

- Keep original table writes and add a compiler-private transposed mirror rather than replacing the source object, preserving output and alias-visible behavior.
- Initialize each diagonal at its descending outer-row entry and mirror both zero-trip and nonzero-trip middle iterations.
- Cap bound-derived shadow storage at 16 MiB and transform at most one candidate per module.
- Treat local QEMU timing as inconclusive because it does not model the cache-locality mechanism; use official Cortex-A53 performance for keep/revert.

## Best valid result

The hot second load is contiguous with the same hot-loop instruction count. Official Cortex-A53 h-8 time is 10.64s total versus 34.35s before the shadow, about 3.23x faster; the legality fix does not alter those assemblies.

## Active experiments

- Commit and push the isolated must-execute legality fix.

## Blocked or uncertain

- The checked-out public test copies currently lack `.out` files; full local runs therefore report `NO_EXPECTED` despite successful compile/link/run. Assembly differential against the accepted parent is used as the exact isolation check.

## Next actions

Commit and push the isolated legality fix to `zct-arm-no-magic` after the final parent/candidate assembly differential completes.

## Files and commands to reread

- `src/midend/pass/TriangularTransposeShadowPass.cpp`
- `tests/optimization/triangular_transpose_shadow.sy`
- `tests/optimization/triangular_transpose_shadow_fallback.sy`
- `tests/optimization/triangular_transpose_shadow_control_flow_fallback.sy`
- `/tmp/eval-transpose-final-performance-60`
- `/tmp/eval-transpose-final-correctness-140`
