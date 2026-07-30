# Research or competition checkpoint

## Current objective

Publish one locally validated transposed-shadow candidate to the owner-designated AArch64 delivery branch and await the official Cortex-A53 result.

## Confirmed facts

- Loop scalar promotion already removes the repeated destination load/store, but h-8 retains an O(B^3) row-stride dependency load.
- The final pass matches only a constant-bound descending triangular schedule, exact affine accesses, and a no-call/no-aliasing-store outer region.
- Anonymous 9x9 renamed/reformulated tests pass, including zero-trip inner loops, i32 wrapping values, and two sequential calls.
- A structurally similar 11x11 case with an extra matrix write falls back.
- The 60-case scan transforms only h-8-01/02/03.
- Final gates: correctness 140/140 AC; performance 57 AC plus the three already documented h-4 evaluator formatting exceptions. h-4 enabled and disabled assembly are byte-identical with SHA-256 1e7e46211863afd4643b72e689f5c9cfd93187b19bca82241f4f16d8384ba433.

## Decisions and rationale

- Keep original table writes and add a compiler-private transposed mirror rather than replacing the source object, preserving output and alias-visible behavior.
- Initialize each diagonal at its descending outer-row entry and mirror both zero-trip and nonzero-trip middle iterations.
- Cap bound-derived shadow storage at 16 MiB and transform at most one candidate per module.
- Treat local QEMU timing as inconclusive because it does not model the cache-locality mechanism; use official Cortex-A53 performance for keep/revert.

## Best valid result

The hot second load is now contiguous with the same hot-loop instruction count. Local three-case internal timing was 4.275252s enabled versus 4.277266s disabled, effectively flat as expected under QEMU.

## Active experiments

- Official Cortex-A53 submission of the pushed candidate: pending repository owner.

## Blocked or uncertain

- Meaningful target improvement is unknown until official Cortex-A53 evaluation.

## Next actions

Commit and push the isolated candidate. Keep only if the official result is meaningfully better; otherwise revert and push promptly.

## Files and commands to reread

- `src/midend/pass/TriangularTransposeShadowPass.cpp`
- `tests/optimization/triangular_transpose_shadow.sy`
- `tests/optimization/triangular_transpose_shadow_fallback.sy`
- `/tmp/eval-transpose-final-performance-60`
- `/tmp/eval-transpose-final-correctness-140`
