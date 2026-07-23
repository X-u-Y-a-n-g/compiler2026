# Object-Sensitive Memory Value Forwarding

## Transformation

The optimized `-O1` load-forwarding pass assigns memory accesses a semantic
base object when the address is proven to derive from a global object or an
`alloca`. A store invalidates available values from the same object and from
unknown objects, but preserves values belonging to proven-distinct objects.
Stores to non-escaping scalar stack objects also preserve unknown external
loads because those stack addresses cannot be reached by an external pointer.

Calls use the existing interprocedural `FunctionEffectAnalysis`. Read-only and
read-none calls preserve available values. Calls that may write external
memory invalidate every value except values in non-escaping scalar stack
objects. Unknown addresses and mixed-object `phi`/`select` values retain the
conservative fallback.

The analysis does not inspect source paths, benchmark names, function names,
global names, fixed array sizes, constants, inputs, or expected results. A
global symbol is used only as an opaque object identity for alias analysis.

## Validation

- Synthetic AArch64 candidate/baseline comparison covers distinct globals,
  distinct stack objects, same-object stores, unknown pointer stores,
  read-none calls, calls that write a global, and stack stores between unknown
  pointer loads.
- AArch64 performance regression: 60/60 AC.
- Exactly nine final assemblies change: the three sort, Huffman, and shuffle
  programs. The other 51 assemblies are byte-identical to the disabled
  baseline.
- Across the changed assemblies: 57 fewer loads, three fewer calls, no change
  in store count, and 153 fewer assembly lines.
- Three alternating local AArch64 QEMU diagnostic rounds for those nine cases:
  baseline `0.493609/0.489961/0.460994s`, candidate
  `0.436369/0.432151/0.428589s`.

Official Cortex-A53 timing remains the final acceptance authority.

## Assistance and references

OpenAI Codex assisted with IR inspection, legality conditions, implementation,
synthetic tests, assembly comparison, and local diagnostics. The technical
basis is standard base-object alias analysis, available-load forwarding,
memory value numbering, and interprocedural read/write effect propagation; no
external compiler source code was copied.
