# Tail-recursion memo miss checkpoint candidate

Date: 2026-07-31

## Transformation and proof

This candidate applies only to the existing compact dense memoization path for
a leaf, side-effect-free, additive tail recurrence whose sole scalar memory
context has already been proved immutable for the entire program lifetime.
The existing cache maps a positive state to `remaining_depth + 1`; zero remains
the empty encoding. Its `2^24` state window and entry format are unchanged.

On every in-window empty lookup, the generated loop records that state and the
current additive depth in two otherwise unused caller-saved registers. The
latest such miss is therefore the closest uncached state to the eventual base
case or cache hit. When the traversal returns, the compiler writes both the
original call-entry summary and this checkpoint summary:

`checkpoint_delta = final_depth - checkpoint_depth`

The subtraction and encoding use the same i32 modulo arithmetic as the
existing entry. A cache hit at the checkpoint consequently reconstructs the
same final result as executing the omitted suffix. Out-of-window and
non-positive states remain on the original path. Multi-context tail summaries,
general read-only helper memoization, and branching-recursion memoization are
unchanged.

This progressively fills a trajectory from both ends without a path buffer,
an additional probe, a larger cache, or runtime input inspection. The two
register moves execute only after an already-established empty cache lookup.
`COMPILER_DISABLE_TAIL_MEMO_MISS_CHECKPOINT=1` independently restores the
accepted single-entry behavior.

The matcher additionally proves that `w8/x8` and `w9/x9` are unused throughout
the transformed leaf function before reserving them. It still rejects calls,
stores, additional memory reads, non-additive uses of the depth, mutable
context, and scratch-register conflicts.

## Acceptance status

Docker is installed but its daemon is unavailable. Under the repository's
no-Docker exception and the owner's explicit instruction, no local build,
QEMU run, or timing test was performed. Static review covered base-case return,
cache-hit return, entry outside the cache followed by an in-range checkpoint,
negative/non-positive fallback, repeated empty misses, i32 wraparound, an
already cached terminal state, scratch conflicts, and the disable path.

The parent official Cortex-A53 result is `h-1-01/02/03 =
11.91/1.46/6.55s`. The candidate must be kept only after a correct, meaningful
official improvement; flat or regressed results require an isolated revert.

## Assistance and references

AI assistance covered the trajectory-summary proof, assembly construction,
register audit, fallback analysis, and documentation. No external source code
or benchmark-specific trigger was used. The technical basis is classical
memoization/path compression and the AArch64 caller-saved register convention.
