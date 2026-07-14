# Official BOOM FPGA Performance Workflow

This document is the persistent handoff for accessing the official 2026 RISC-V
BOOM FPGA board. It records the workflow that was exercised successfully on
2026-07-15 from macOS and the `compiler-impl2026-dev` Docker container.

## Proven result

The complete path was verified with `tests/performance/transpose0.sy`:

- board: `PYNQ-15EG-50`, BOOM v3, RV64GC, 50 MHz;
- generated output: `135935384\n`;
- return value: `0`;
- official expected output and return value: identical;
- timed interval: `37,619,737` cycles, about `0.75239474s` at 50 MHz.

The timer is read with `rdcycle`, so it measures target execution rather than
GDB transfer time or host wall-clock time.

## Components

- Web UI: allocate the FPGA board and start/stop the remote OpenOCD service.
- Host client: `/Users/zhouzhongtian/Downloads/client-darwin` forwards the
  remote GDB WebSocket to a local TCP port.
- Docker container: holds the compiler, RISC-V cross tools, and
  `gdb-multiarch`.
- Bare-metal helper: `scripts/fpga/` supplies startup code, a minimal SysY
  runtime, a linker script, and build/run helpers.

The Linux static executable produced by `scripts/eval.py` is not suitable for
this board. OpenOCD downloads directly into a bare RV64 machine-mode target;
there is no Linux syscall environment. The verified image starts at
`0x90000000`, reads input from `0xc0000000`, and reports results through a
reserved memory block at `0xcf000000`.

## One-time preparation

1. Open the contest online debugging environment and allocate a board.
2. Click `启动Riscv GDB`. The log must contain `Listening on port 3333 for gdb connections`.
3. On macOS, start the client in a dedicated terminal and leave it running:

   ```bash
   /Users/zhouzhongtian/Downloads/client-darwin
   ```

4. In the web UI, click `搜索客户端`. Confirm both of these signals:

   - `端口转发客户端已连接`
   - `GDB Server tcp://127.0.0.1:3333`

5. Confirm the host listener:

   ```bash
   lsof -nP -iTCP:3333 -sTCP:LISTEN
   ```

From inside Docker, the forwarding endpoint is
`host.docker.internal:3333`. A read-only connectivity smoke test is:

```bash
docker exec compiler-impl2026-dev bash -lc '
  timeout 15s gdb-multiarch -q -nx -batch \
    -ex "set architecture riscv:rv64" \
    -ex "target remote host.docker.internal:3333" \
    -ex "info registers pc"'
```

## Build a board ELF

All compiler builds and target builds must run in the container. Generate the
performance assembly with the explicit current compiler:

```bash
docker exec compiler-impl2026-dev bash -lc '
  cd /workspace
  /workspace/build/compiler -S -O1 \
    -o /tmp/transpose0.s \
    tests/performance/transpose0.sy
  scripts/fpga/build_elf.sh \
    /tmp/transpose0.s \
    /tmp/transpose0.elf'
```

The helper uses `-nostdlib`, the bare-metal startup/runtime, RV64GC, the double
float ABI, and a linker base of `0x90000000`. It also checks that the resulting
ELF entry and load segment are valid.

## Run one sample

The official tutorial sequence is `target remote`, `restore input`, `load`,
then `continue`. The helper performs that sequence and stops at `__done`:

```bash
docker exec compiler-impl2026-dev bash -lc '
  cd /workspace
  scripts/fpga/run_gdb.sh \
    /tmp/transpose0.elf \
    tests/performance/transpose0.in'
```

The input is copied and NUL-terminated before being restored to
`0xc0000000`. After the program stops, GDB prints:

- `0xcf000000`: low 32 bits are the `main` return value, next 32 bits are the
  completion marker (`1`);
- `0xcf000008`: accumulated `starttime`/`stoptime` cycles;
- `0xcf000018`: captured output length;
- `0xcf000100`: captured output bytes/string.

At 50 MHz, convert cycles to seconds with `cycles / 50000000.0`. For an
optimization iteration, measure the selected hotspot sample once on the current
baseline and once on the candidate, using cycles as the primary metric. Do not
spend board time repeatedly alternating revisions unless the result is noisy or
otherwise suspicious. Keep the startup/runtime/linker layout identical: moving
the same target code changed the measured cycle count enough to make absolute
results from differently linked ELF files incomparable.

The acceptance target is target-program execution time on the FPGA. A candidate
must produce a meaningful hotspot reduction before broad validation. Host or
QEMU timing is optional supporting evidence, not an acceptance gate. Run the
full performance suite only when the expected aggregate impact justifies the
board time; after an accepted FPGA speedup, verify correctness before committing
and pushing it to `zct`.

## Mandatory reset between runs

The current `client-darwin` and remote OpenOCD path behaves as a single GDB
session. After GDB detaches, the listener may remain visible while its WebSocket
is already closed. Reusing it produces `Remote connection closed` or
`Network is unreachable`.

Before every subsequent run:

1. Stop the host client with `Ctrl-C`.
2. In the web UI, click `关闭Riscv GDB`, then `启动Riscv GDB`.
3. If the UI still says the client is connected, click `断开连接`.
4. Start `/Users/zhouzhongtian/Downloads/client-darwin` again.
5. Click `搜索客户端` and confirm the `3333` forwarding row.
6. Run exactly one `scripts/fpga/run_gdb.sh` session.

The page implements the search action by POSTing a proxy description to the
client at `http://127.0.0.1:4617/proxy`. For diagnosis only, the equivalent
payload is:

```json
[
  {
    "local_port": 3333,
    "remote_port": 3333,
    "type": "tcp",
    "desc": "GDB Server",
    "ws_url": "wss://fpga.educg.net/api/proxy?board=<BOARD_HOSTNAME>&port=3333"
  }
]
```

Prefer the web UI because it supplies the allocated board hostname and reflects
the current allocation state.

## Current runtime scope

The committed minimal runtime supports the integer interfaces needed by
`transpose0` and `many_mat_cal`: `getint`, `getch`, `getarray`, `putint`,
`putch`, `putarray`, `_sysy_starttime`, and `_sysy_stoptime`.

Before using a sample that calls floating-point I/O or `putf`, extend the
runtime with the corresponding general SysY ABI functions and validate them on
small synthetic inputs. Do not infer correctness from a sample that never calls
the missing interface.

## Troubleshooting

- `端口转发客户端未连接`: start the host client, then click `搜索客户端`.
- Host `3333` is absent: restart the client and reconnect from the web UI.
- Container cannot use `127.0.0.1:3333`: use `host.docker.internal:3333`.
- `Remote connection closed` on a second run: perform the full reset sequence.
- GDB asks about debuginfod: `run_gdb.sh` disables it.
- Program never reaches `__done`: inspect `pc`, the generated ELF memory map,
  stack/BSS overlap, missing runtime symbols, and input termination.
- Wrong timing: read `0xcf000008`; do not include GDB `restore` or `load` wall
  time in the performance number.
