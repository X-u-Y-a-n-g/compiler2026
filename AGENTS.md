# Competition Compliance Rules

These rules are mandatory for every compiler optimization in this repository.

## Allowed optimization inputs

- General IR or Machine IR semantics.
- Control-flow, dominance, loop, data-flow, alias, escape, and effect analysis.
- Target-independent cost models and documented RISC-V ISA or microarchitecture properties.
- Source-language rules that hold for every valid program covered by the transformation.

## Prohibited optimization inputs

- Input paths, filenames, benchmark names, source text fingerprints, comments, or timer line numbers.
- User-defined function, variable, global, label, or generated basic-block names, except for language/runtime ABI handling such as `main`, `starttime`, and `stoptime` where required by the specification.
- A fixed public-test array size, argument count, constant, CFG fingerprint, input range, known output, or expected algorithm result used to identify a workload.
- Runtime probing of judge inputs, host properties, timing behavior, environment layout, or other evaluation characteristics.
- Undefined behavior, unsupported assumptions, or speculative transformations that are not correct for every input satisfying the stated preconditions.
- Replacing a recognized public program with a different algorithm. A loop or algebraic transform must be independently defined and applicable to all IR with the same proven semantics.

## Required acceptance process

1. State the transformation using only general semantic and analysis preconditions.
2. Test structurally varied synthetic cases, boundary values, zero-trip loops, aliasing, overflow, and fallback paths as applicable.
3. Build and evaluate inside `compiler-impl2026-dev` with `/workspace/build/compiler`.
4. Run all 60 performance cases first and compare assembly, dynamic instructions, and `generic-ooo` evidence for changed hot blocks.
5. Only after a real execution-time improvement, run all 140 correctness cases.
6. Accept and push one optimization at a time only after 60/60 performance correctness and 140/140 correctness pass.
7. Document AI-assisted implementation scope and any external technical references.

If a proposed optimization cannot satisfy these rules, do not implement or restore it.
