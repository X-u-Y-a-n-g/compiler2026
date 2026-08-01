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

## Scope of sample changes

- A request to optimize one sample family defines the measurement focus only. It is not an optimization trigger, a sample-name whitelist, or a requirement that every other program's assembly remain byte-for-byte identical.
- A general transformation may change any program that satisfies its semantic preconditions, including programs affected through ordinary pass interaction. Such changes are allowed when correctness is preserved and the generated code has a documented target-relevant cost reduction or no demonstrated regression.
- Compare all 60 final assemblies to enumerate direct and downstream changes. Byte-for-byte equality is attribution and safety evidence, not a hard acceptance gate. Do not revert a legal positive change solely because another sample changed.
- For every changed program, record whether the change is a direct match or a downstream consequence, and inspect its static/dynamic cost and target result. A static instruction reduction or QEMU improvement alone does not prove official acceptance; clear target regressions must be isolated and removed.

## Required acceptance process

1. State the transformation using only general semantic and analysis preconditions.
2. Test structurally varied synthetic cases, boundary values, zero-trip loops, aliasing, overflow, and fallback paths as applicable.
3. Build and evaluate inside `compiler-impl2026-dev` with `/workspace/build/compiler`.
4. Before publishing a candidate, run all 60 ARM performance cases in `-O1`
   mode. They must remain 60/60 AC, except for an already documented evaluator
   formatting exception whose final assembly and behavior are unchanged from
   the parent revision; such an exception must be reported explicitly rather
   than relabeled AC. Run the 140 ARM correctness cases when a change affects
   the backend, ABI, unoptimized path, frontend/shared semantic code, or
   another risk makes that channel relevant.
5. After the local correctness gates pass, commit and push exactly one
   optimization candidate to the repository-owner-designated ARM delivery
   branch. The repository owner submits that pushed revision to the official
   Cortex-A53 platform; local QEMU and LLVM MCA remain diagnostic evidence only.
   Pushing the locally validated candidate is a prerequisite for that official
   submission, so do not wait for Cortex-A53 results before pushing it.
6. Use the official Cortex-A53 result to decide acceptance. Keep and document a
   meaningful target improvement; promptly revert and push an isolated target
   regression, statistically flat result, or wrong result before developing
   the next candidate.
7. The current owner-designated delivery branch is `zct-arm-no-magic`. Never
   push optimization work to another branch unless the repository owner
   explicitly requests it.
8. Document AI-assisted implementation scope and any external technical references.

If a proposed optimization cannot satisfy these rules, do not implement or restore it.

## Official ARM evaluation

- The active target is AArch64/Cortex-A53; retired RISC-V BOOM FPGA helpers are
  not valid for current acceptance.
- Build and validate candidates inside `compiler-impl2026-dev` with target
  `aarch64`, `aarch64-linux-gnu-gcc`, and `qemu-aarch64`.
- The repository owner performs the official online Cortex-A53 submission from
  the pushed delivery-branch revision and reports the result for keep/revert.
- Do not infer official target improvement from local QEMU timing alone.
