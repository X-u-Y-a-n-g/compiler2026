# AArch64 indexed SIMD-lane coefficient loads

Date: 2026-08-05

Status: published candidate; official Cortex-A53 result pending.

## General transformation

The accepted ordered in-place row-recurrence lowering has two shared-source
regions.  For each reduction index, it loads eight independent i32
coefficients, broadcasts each coefficient to four lanes, and applies the
coefficient to two i32x4 accumulators.  This candidate changes only those
already-proven regions:

```text
ldr Wcoefficient, [Xrow, Windex, sxtw #2]
dup Vcoefficient.4s, Wcoefficient
mla Vaccumulator.4s, Vsource.4s, Vcoefficient.4s
```

becomes:

```text
ldr Scoefficient, [Xrow, Windex, sxtw #2]
mla Vaccumulator.4s, Vsource.4s, Vcoefficient.s[0]
```

Eight different SIMD registers hold the eight coefficients, so all loads
remain before the multiply-add group and retain the parent's latency overlap.
The transformation is controlled by
`COMPILER_DISABLE_AARCH64_INDEXED_LANE_COEFFICIENTS=1` for exact isolation.

## Semantic and compliance preconditions

- The rule is emitted only inside the existing AArch64 8-row by 8-column
  ordered panel after that transform has proved canonical zero-based unit-step
  loops, two-dimensional i32 objects, distinct coefficient and destination
  roots, strict row-version dependencies, no intervening clobber, and complete
  fallback paths.
- The scalar `LDR W` and SIMD scalar `LDR S` read the same four bytes from the
  same effective address.  `MLA (by element)` uses those bits as the same i32
  coefficient for every destination lane.  Integer multiplication and
  addition therefore retain the original modulo-2^32 result.
- The candidate does not inspect paths, source text, comments, user symbols,
  generated block names, public dimensions, inputs, outputs, or timing data.
  The public cases are a measurement focus only.
- Zero-trip loops, partial row groups, non-multiples of eight, alias/effect
  failures, and unsupported CFG shapes keep the existing fallback.  The new
  instruction form is reached only after the same runtime panel guards as the
  accepted parent.

## Local validation

Docker was queried on 2026-08-05.  The daemon was unavailable, and no local
AArch64 assembler, linker, or QEMU was installed.  Under the repository's
no-Docker exception, Docker-dependent 60-case execution and the 140-case
correctness gate were skipped rather than represented as passing.

- All four modified C++ translation units pass `g++ -std=c++17 -Wall -Wextra
  -Wpedantic -fsyntax-only`; the reported indentation warnings predate this
  candidate.
- A complete local compiler build succeeds, and both enabled and disabled
  modes compile all 60 performance sources to AArch64 assembly.
- Exact SHA-256 comparison of the 60 enabled/disabled assemblies changes only
  `many_mat_cal-1/2/3`.  All three are direct structural matches; the other 57
  are byte-for-byte identical, so there are no downstream changes.
- In every direct match, each shared prefix/high loop body changes from 41 to
  30 static instructions: eight scalar loads and eight broadcasts become
  eight SIMD scalar loads, while three coefficient-row stack reloads also
  disappear after register allocation.  The main stack frame changes from
  352 to 304 bytes.  The whole assembly changes from 1676 to 1520 lines.
- Existing structurally varied ordered-panel tests cover dynamic zero/small/
  full trips, non-divisible fallback, i32 wraparound, and observable-effect or
  unsupported-shape fallback.  This candidate does not change their guards or
  dependency bodies; enabled and disabled compilation both succeed.

Static instruction reduction is diagnostic evidence only.  Acceptance still
requires the repository owner's official Cortex-A53 run.  A meaningful target
improvement keeps this candidate; a wrong result, regression, or statistically
flat result requires an isolated revert and push before further work.

## Technical references and AI assistance

The instruction selection follows Arm's A64 ISA documentation for register-
offset loads, SIMD scalar/vector register views, and vector-by-element
operations:

- https://developer.arm.com/-/media/Files/pdf/graphics-and-multimedia/ARMv8_InstructionSetOverview.pdf
- https://developer.arm.com/documentation/102374/0103/Registers-in-AArch64---general-purpose-registers

AI assistance covered opportunity discovery, legality analysis, implementation,
the exact switch-disabled assembly comparison, and documentation.  No external
implementation was copied.
