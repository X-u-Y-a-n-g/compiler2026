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
4. Validate the selected hotspot on the official FPGA first. Use target `rdcycle`
   as the primary performance metric; local QEMU, dynamic instructions, and
   `generic-ooo` are optional diagnostic evidence.
5. After a meaningful FPGA speedup, run all 60 performance cases in `-O1` mode.
   They must remain 60/60 AC before accepting the optimization.
6. The official 140 correctness cases do not enable the optimization mode. Run
   them only when a change affects the unoptimized path, frontend/semantic
   shared code, or another risk makes that channel relevant.
7. Accept and push one optimization at a time to `zct`; never push optimization
   work to `wz`.
8. Document AI-assisted implementation scope and any external technical references.

If a proposed optimization cannot satisfy these rules, do not implement or restore it.

## Official FPGA access

- Read `docs/fpga-performance.md` before using the official BOOM FPGA board.
- Build target assembly and ELF inside `compiler-impl2026-dev`; use
  `scripts/fpga/build_elf.sh` for the bare-metal image and
  `scripts/fpga/run_gdb.sh` for one GDB run.
- The macOS forwarding client runs on the host. The container connects to it at
  `host.docker.internal:3333`, not `127.0.0.1:3333`.
- The client/OpenOCD path is effectively one GDB session per setup. Before each
  new run, close and restart RISC-V GDB in the web UI, restart the client, and
  reconnect the port forward.
- Do not load the Linux static ELF produced by `scripts/eval.py` onto the FPGA.
  The BOOM target is bare metal and uses the linker/runtime under `scripts/fpga/`.
- The proven smoke test is `tests/performance/transpose0.sy`: output
  `135935384`, return value `0`, and `37,619,737` timed cycles on the allocated
  50 MHz BOOM board on 2026-07-15.
