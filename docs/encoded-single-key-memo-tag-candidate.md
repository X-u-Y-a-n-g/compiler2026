# Encoded single-key memo tag candidate

Date: 2026-07-31

## Transformation and semantic preconditions

This candidate applies only to the existing phase-local memoization of a
read-only integer helper when all prior lifetime/effect checks have succeeded
and the helper has exactly one i32 parameter. The call site must still be the
unique call inside a loop, and the helper's transitive global read set must
remain immutable throughout that loop. The cache remains direct-mapped with
the same `2^17` capacity, full-key validation, overwrite policy, value array,
and original-CFG fallback.

The former metadata uses separate i32 `valid[index]` and `key[index]` arrays.
For a single i32 key, the candidate instead stores one i64 tag:

`tag = zext_i64(key) + 1`

All 2^32 key bit patterns therefore map bijectively to `1..2^32`; zero is the
unique empty tag. Lookup computes the expected tag, loads one i64 element, and
compares it directly. A mismatch executes the complete original helper. On a
miss, the result is stored before the tag, preserving the existing publication
order. Negative keys, `INT_MIN`, `INT_MAX`, zero, direct-map collisions, and a
cached result equal to zero are all represented without a sentinel conflict.

The tag array and separate i32 value array consume the same 12 bytes per slot
as the previous valid/key/value structure. Unlike rejected packed records, the
value is not fetched on a miss and the cache capacity is not reduced. Unlike
the rejected second probe, no additional random address or collision branch is
introduced. A hit or occupied-slot collision replaces two metadata loads and
two conditional branches with one metadata load and one comparison branch; an
empty-slot miss retains one metadata load/branch while avoiding the extra
check block.

Multi-parameter helpers, branching-recursion caches, tail-recursion summaries,
and helpers without the proven loop-lifetime read-only property retain the
previous representation. `COMPILER_DISABLE_ENCODED_SINGLE_KEY_MEMO_TAG=1`
independently restores it.

## Acceptance status

Docker is installed but its daemon is unavailable. Under the repository's
no-Docker exception and the owner's explicit instruction, no local build,
QEMU execution, or timing test was performed. Static review covered every i32
key bit pattern, empty slots, collisions, zero results, store ordering, cache
hits and misses, and the multi-key/disabled fallbacks.

The parent official Cortex-A53 result is `shuffle0/1/2 = 7.59/14.85/0.15s`;
the older accepted-reference result before recent evaluation noise was
`7.02/14.97/0.15s`. Acceptance requires 60/60 correctness and a meaningful
official improvement of the primary `shuffle1` target without a material
regression in the other two cases. Otherwise this candidate must be isolated-
reverted before further work.

## Assistance and references

AI assistance covered the bijective tag proof, IR construction, cache-layout
accounting, fallback audit, and documentation. No external implementation was
copied. The technical basis is tagged cache metadata, zero extension, and
release-by-program-order publication in a single-threaded program.
