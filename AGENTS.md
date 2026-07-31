# Competition Compliance Rules

These rules are mandatory for every compiler optimization in this repository.

## Allowed optimization inputs

- General IR or Machine IR semantics.
- Control-flow, dominance, loop, data-flow, alias, escape, and effect analysis.
- Target-independent cost models and documented AArch64 ISA or Cortex-A53
  microarchitecture properties for the active ARM track.
- Source-language rules that hold for every valid program covered by the transformation.

## Prohibited optimization inputs

- Input paths, filenames, benchmark names, source text fingerprints, comments, or timer line numbers.
- User-defined function, variable, global, label, or generated basic-block names, except for language/runtime ABI handling such as `main`, `starttime`, and `stoptime` where required by the specification.
- Runtime probing of judge inputs, host properties, timing behavior, environment layout, or other evaluation characteristics.
- Undefined behavior, unsupported assumptions, or speculative transformations that are not correct for every input satisfying the stated preconditions.

## Required acceptance process

1. State the transformation using only general semantic and analysis preconditions.
2. Test structurally varied synthetic cases, boundary values, zero-trip loops, aliasing, overflow, and fallback paths as applicable.
3. When Docker is available locally, build and evaluate inside
   `compiler-impl2026-dev` with `/workspace/build/compiler`. If Docker is not
   available locally, document that fact, skip the Docker-dependent local
   validation gates, and commit and push the candidate directly to `yang-arm`
   for official evaluation.
4. When Docker is available locally, before publishing a candidate, run all 60
   ARM performance cases in `-O1` mode. They must remain 60/60 AC, except for
   an already documented evaluator formatting exception whose final assembly
   and behavior are unchanged from the parent revision; such an exception must
   be reported explicitly rather than relabeled AC. Run the 140 ARM correctness
   cases when a change affects the backend, ABI, unoptimized path,
   frontend/shared semantic code, or another risk makes that channel relevant.
5. After the applicable local correctness gates pass—or immediately when the
   no-Docker exception in step 3 applies—commit and push exactly one
   optimization candidate to `yang-arm`. The repository owner submits that
   pushed revision to the official Cortex-A53 platform; local QEMU and LLVM MCA
   remain diagnostic evidence only. Do not wait for Cortex-A53 results before
   pushing it.
6. Use the official Cortex-A53 result to decide acceptance. Keep and document a
   meaningful target improvement; promptly revert and push an isolated target
   regression, statistically flat result, or wrong result before developing
   the next candidate.
7. The current owner-designated delivery branch is `yang-arm`. Never
   push optimization work to another branch unless the repository owner
   explicitly requests it.
8. Document AI-assisted implementation scope and any external technical references.

If a proposed optimization cannot satisfy these rules, do not implement or restore it.

## Official ARM evaluation

- The active target is AArch64/Cortex-A53; retired RISC-V BOOM FPGA helpers are
  not valid for current acceptance.
- When Docker is available locally, build and validate candidates inside
  `compiler-impl2026-dev` with target `aarch64`, `aarch64-linux-gnu-gcc`, and
  `qemu-aarch64`; otherwise use the no-Docker exception in step 3.
- The repository owner performs the official online Cortex-A53 submission from
  the pushed delivery-branch revision and reports the result for keep/revert.
- Do not infer official target improvement from local QEMU timing alone.
