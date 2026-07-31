# Paired-K conditional matrix reduction candidate

Date: 2026-07-31

## Transformation and semantic preconditions

This candidate extends the existing AArch64 conditional integer matrix
reduction only after that matcher has proved:

- three distinct global objects (two read-only inputs and one output);
- identical static square extents, a unique output store, and an extent
  divisible by eight;
- the canonical `i-j-k` i32 additive reduction and its exact low-bit parity
  predicate;
- the already accepted `i-k-j` schedule, which keeps each output element's
  contributions in increasing K order.

The new micro-kernel pairs adjacent K iterations. For each eight-column output
vector it loads the accumulator once, applies K and K+1 in their original
order, and stores once. The two source rows are still traversed as contiguous
streams. Four loop bodies cover the even/even, odd/even, even/odd, and odd/odd
predicate combinations. The parity identity and all vector arithmetic use
i32 modulo-2^32 semantics, so negative operands and signed overflow have the
same result as the original scalar operations.

The paired path requires an even K extent and reserves one additional vector
register. If either condition is unavailable, the existing single-K kernel is
retained. `COMPILER_DISABLE_PAIRED_K_MATRIX_REDUCTION=1` independently disables
the candidate.

This is deliberately not the previously rejected J-panel/full-K interchange.
Only two consecutive rows are active, so every input row remains a single
forward stream while output load/store traffic is halved.

## Acceptance status

Docker is installed but its daemon is unavailable in this workspace. Following
the repository's no-Docker exception and the owner's explicit instruction, no
local build, QEMU run, or timing test was performed. Static review covered:

- minimum eligible width (8) and larger widths;
- all four parity combinations, including negative odd/even values;
- i32 wraparound and the preserved K contribution order;
- register-scratch isolation and the disable/fallback path;
- rejection of non-eight-wide or conflicting-vector-register functions.

The parent official Cortex-A53 baseline is `matmul1/2/3 =
4.68/4.69/4.69s`. The candidate must be kept only if the official run is
correct and meaningfully improves these targets; flat or regressed results
require an isolated revert before another candidate.

## Assistance and references

AI assistance covered the semantic proof, AArch64 micro-kernel construction,
four-way parity specialization, scratch-register audit, and documentation.
No external source code or benchmark-specific trigger was used. The technical
basis is standard loop unroll-and-jam, AArch64 NEON `MLA`, paired loads/stores,
and Cortex-A53-oriented sequential cache locality.
