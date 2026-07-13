#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import time
from collections import Counter
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BENCH_ROOT = REPO_ROOT / "benchmarks" / "nscscc-implementation" / "2026"
DEFAULT_OUTPUT_ROOT = REPO_ROOT / ".eval-2026"
DEFAULT_ANALYSIS_RUNNER = Path("/opt/qemu-8.2.2-plugin/bin/qemu-riscv64")
DEFAULT_TIMED_PROFILE_PLUGIN = Path(
    "/opt/qemu-8.2.2-plugin/lib/qemu/plugins/libtimedprofile.so"
)
CHANNEL_SUITES = {
    "correctness": ("functional", "h_functional"),
    "performance": ("performance",),
}
TOTAL_PATTERN = re.compile(
    r"TOTAL:\s*(\d+)H-(\d+)M-(\d+)S-(\d+)us", re.MULTILINE
)
ASM_INSTRUCTION_PATTERN = re.compile(r"^\s+([A-Za-z][A-Za-z0-9_.]*)\s*(.*?)\s*$")
ASM_LOAD_MNEMONICS = {"lb", "lbu", "lh", "lhu", "lw", "lwu", "ld", "flw", "fld"}
ASM_STORE_MNEMONICS = {"sb", "sh", "sw", "sd", "fsw", "fsd"}
ASM_BRANCH_MNEMONICS = {
    "beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez",
    "blez", "bgez", "bltz", "bgtz", "j", "jr", "ret",
}
DYNAMIC_INSN_PATTERN = re.compile(r"timed insns:\s*(\d+)")
HOTBLOCK_PATTERN = re.compile(
    r"^0x([0-9a-fA-F]+),\s*(\d+),\s*(\d+)\s*$", re.MULTILINE
)
MCA_VALUE_PATTERNS = {
    "mca_iterations": re.compile(r"^Iterations:\s*(\d+)", re.MULTILINE),
    "mca_instruction_count": re.compile(r"^Instructions:\s*(\d+)", re.MULTILINE),
    "mca_total_cycles": re.compile(r"^Total Cycles:\s*([0-9.]+)", re.MULTILINE),
    "mca_ipc": re.compile(r"^IPC:\s*([0-9.]+)", re.MULTILINE),
    "mca_block_rthroughput": re.compile(
        r"^Block RThroughput:\s*([0-9.]+)", re.MULTILINE
    ),
}

# The public SysY2022 runtime linked from the 2026 contest page. Keeping the
# runtime here makes the evaluator self-contained and preserves the official
# starttime()/stoptime() output format consumed by the judge.
SYSY_RUNTIME_C = r'''#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>

int getint(){int t; scanf("%d",&t); return t;}
int getch(){char c; scanf("%c",&c); return (int)c;}
float getfloat(){float n; scanf("%a",&n); return n;}
int getarray(int a[]){int n; scanf("%d",&n); for(int i=0;i<n;i++) scanf("%d",&a[i]); return n;}
int getfarray(float a[]){int n; scanf("%d",&n); for(int i=0;i<n;i++) scanf("%a",&a[i]); return n;}
void putint(int a){printf("%d",a);}
void putch(int a){printf("%c",a);}
void putarray(int n,int a[]){printf("%d:",n); for(int i=0;i<n;i++) printf(" %d",a[i]); printf("\n");}
void putfloat(float a){printf("%a",a);}
void putfarray(int n,float a[]){printf("%d:",n); for(int i=0;i<n;i++) printf(" %a",a[i]); printf("\n");}
void putf(char a[], ...){va_list args; va_start(args,a); vfprintf(stdout,a,args); va_end(args);}

#define _SYSY_N 1024
static struct timeval _sysy_start,_sysy_end;
static int _sysy_l1[_SYSY_N],_sysy_l2[_SYSY_N];
static int _sysy_h[_SYSY_N],_sysy_m[_SYSY_N],_sysy_s[_SYSY_N],_sysy_us[_SYSY_N];
static int _sysy_idx;

__attribute((constructor)) void before_main(){
  for(int i=0;i<_SYSY_N;i++) _sysy_h[i]=_sysy_m[i]=_sysy_s[i]=_sysy_us[i]=0;
  _sysy_idx=1;
}
__attribute((destructor)) void after_main(){
  for(int i=1;i<_sysy_idx;i++){
    fprintf(stderr,"Timer@%04d-%04d: %dH-%dM-%dS-%dus\n",
      _sysy_l1[i],_sysy_l2[i],_sysy_h[i],_sysy_m[i],_sysy_s[i],_sysy_us[i]);
    _sysy_us[0]+=_sysy_us[i];
    _sysy_s[0]+=_sysy_s[i]; _sysy_us[0]%=1000000;
    _sysy_m[0]+=_sysy_m[i]; _sysy_s[0]%=60;
    _sysy_h[0]+=_sysy_h[i]; _sysy_m[0]%=60;
  }
  fprintf(stderr,"TOTAL: %dH-%dM-%dS-%dus\n",_sysy_h[0],_sysy_m[0],_sysy_s[0],_sysy_us[0]);
}
void _sysy_starttime(int lineno){
  _sysy_l1[_sysy_idx]=lineno;
  gettimeofday(&_sysy_start,NULL);
}
void _sysy_stoptime(int lineno){
  gettimeofday(&_sysy_end,NULL);
  _sysy_l2[_sysy_idx]=lineno;
  _sysy_us[_sysy_idx]+=1000000*(_sysy_end.tv_sec-_sysy_start.tv_sec)+_sysy_end.tv_usec-_sysy_start.tv_usec;
  _sysy_s[_sysy_idx]+=_sysy_us[_sysy_idx]/1000000; _sysy_us[_sysy_idx]%=1000000;
  _sysy_m[_sysy_idx]+=_sysy_s[_sysy_idx]/60; _sysy_s[_sysy_idx]%=60;
  _sysy_h[_sysy_idx]+=_sysy_m[_sysy_idx]/60; _sysy_m[_sysy_idx]%=60;
  _sysy_idx++;
}
'''


@dataclass(frozen=True)
class CommandResult:
    return_code: int
    stdout: bytes
    stderr: bytes
    elapsed_sec: float
    timed_out: bool = False


@dataclass(frozen=True)
class CaseResult:
    channel: str
    suite: str
    case: str
    source: str
    final_status: str
    compile_status: str
    compile_exit_code: int
    link_status: str
    link_exit_code: int
    run_status: str
    run_exit_code: int
    compare_status: str
    timer_status: str
    compile_sec: float
    link_sec: float
    run_wall_sec: float
    timer_sec: float | None
    total_wall_sec: float
    asm_instruction_count: int
    asm_load_count: int
    asm_store_count: int
    asm_stack_memory_count: int
    asm_branch_count: int
    asm_call_count: int
    asm_move_count: int
    asm_mul_count: int
    asm_divrem_count: int
    asm_vector_count: int
    asm_max_loop_depth: int
    asm_loop_weighted_instructions: int
    asm_loop_weighted_memory: int
    asm_loop_weighted_branches: int
    dynamic_status: str
    dynamic_instruction_count: int | None
    dynamic_analysis_wall_sec: float | None
    hot_block_pc: str
    hot_block_instruction_count: int | None
    hot_block_execution_count: int | None
    hot_block_dynamic_instruction_count: int | None
    hot_block_dynamic_instruction_share_pct: float | None
    mca_status: str
    mca_model: str
    mca_iterations: int | None
    mca_instruction_count: int | None
    mca_total_cycles: float | None
    mca_cycles_per_iteration: float | None
    mca_ipc: float | None
    mca_block_rthroughput: float | None
    mca_estimated_hot_cycles: float | None


PROFILE_COMPARISON_FIELDS = (
    "dynamic_instruction_count",
    "dynamic_analysis_wall_sec",
    "hot_block_instruction_count",
    "hot_block_execution_count",
    "hot_block_dynamic_instruction_count",
    "hot_block_dynamic_instruction_share_pct",
    "mca_iterations",
    "mca_instruction_count",
    "mca_total_cycles",
    "mca_cycles_per_iteration",
    "mca_ipc",
    "mca_block_rthroughput",
    "mca_estimated_hot_cycles",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "2026 编译系统实现赛 RISC-V 本地评测：correctness 对应 "
            "functional+h_functional，performance 对应 performance。"
        )
    )
    parser.add_argument(
        "channel",
        choices=("correctness", "performance", "all"),
        help="评测通道：正确性、性能或全部",
    )
    parser.add_argument("--compiler", type=Path, default=None, help="compiler 可执行文件路径")
    parser.add_argument(
        "--bench-root", type=Path, default=DEFAULT_BENCH_ROOT, help="2026 用例根目录"
    )
    parser.add_argument("--output-dir", type=Path, default=None, help="本次结果输出目录")
    parser.add_argument(
        "--sample",
        action="append",
        default=[],
        help="按文件名或相对路径筛选，可重复传入或用逗号分隔",
    )
    parser.add_argument("--limit", type=int, default=0, help="每个通道最多运行 N 个用例，0 为全部")
    parser.add_argument("--compile-timeout", type=int, default=60)
    parser.add_argument("--link-timeout", type=int, default=60)
    parser.add_argument("--run-timeout", type=int, default=300)
    parser.add_argument("--linker", default="riscv64-linux-gnu-gcc")
    parser.add_argument("--runner", default="qemu-riscv64")
    parser.add_argument(
        "--analysis",
        choices=("none", "dynamic", "full"),
        default="none",
        help=(
            "独立分析入口：dynamic 记录 QEMU 动态指令和热点基本块；"
            "full 额外用 LLVM MCA 分析最热基本块。分析运行不参与计时"
        ),
    )
    parser.add_argument(
        "--analysis-runner",
        type=Path,
        default=DEFAULT_ANALYSIS_RUNNER,
        help="启用插件的 qemu-riscv64 路径",
    )
    parser.add_argument(
        "--timed-profile-plugin",
        type=Path,
        default=DEFAULT_TIMED_PROFILE_PLUGIN,
        help="只统计 SysY 计时区间的 QEMU 动态分析插件路径",
    )
    parser.add_argument("--analysis-timeout", type=int, default=900)
    parser.add_argument(
        "--objdump", default="riscv64-linux-gnu-objdump", help="RISC-V 反汇编工具"
    )
    parser.add_argument(
        "--llvm-mca", default="llvm-mca-21", help="LLVM MCA 可执行文件"
    )
    parser.add_argument("--mca-model", default="generic-ooo")
    parser.add_argument("--mca-iterations", type=int, default=100)
    parser.add_argument(
        "--baseline-results",
        type=Path,
        default=None,
        help="上一版 results.tsv 或其目录；生成逐用例时间与汇编指标差异表",
    )
    parser.add_argument(
        "--verify-output",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="将 stdout 和程序返回值与 .out 对拍，默认开启",
    )
    parser.add_argument(
        "--keep-artifacts",
        action=argparse.BooleanOptionalAction,
        default=False,
        help="保留全部汇编和可执行文件；默认只保留失败现场",
    )
    return parser.parse_args()


def resolve_compiler(requested: Path | None) -> Path:
    if requested is not None:
        resolved = requested.expanduser().resolve()
        if resolved.is_file() and os.access(resolved, os.X_OK):
            return resolved
        raise SystemExit(f"错误: 找不到可执行 compiler: {resolved}")

    candidates = (
        REPO_ROOT / ".docker-build" / "compiler",
        REPO_ROOT / "build" / "compiler",
        REPO_ROOT / ".local-build" / "compiler",
    )
    available = [
        candidate.expanduser().resolve()
        for candidate in candidates
        if candidate.expanduser().resolve().is_file()
        and os.access(candidate.expanduser().resolve(), os.X_OK)
    ]
    if available:
        return max(available, key=lambda path: path.stat().st_mtime_ns)
    searched = "\n  ".join(str(path) for path in candidates)
    raise SystemExit(f"错误: 找不到可执行 compiler，已搜索:\n  {searched}")


def split_filters(values: Iterable[str]) -> list[str]:
    return [part.strip() for value in values for part in value.split(",") if part.strip()]


def natural_key(text: str) -> list[tuple[int, object]]:
    return [
        (0, int(part)) if part.isdigit() else (1, part.lower())
        for part in re.split(r"(\d+)", text)
        if part
    ]


def discover_cases(
    bench_root: Path, channel: str, filters: list[str], limit: int
) -> list[tuple[str, Path]]:
    cases: list[tuple[str, Path]] = []
    for suite in CHANNEL_SUITES[channel]:
        suite_dir = bench_root / suite
        if not suite_dir.is_dir():
            raise SystemExit(f"错误: 找不到 2026 用例目录 {suite_dir}")
        sources = sorted(suite_dir.glob("*.sy"), key=lambda path: natural_key(path.name))
        for source in sources:
            rel = f"{suite}/{source.name}"
            if filters and not any(item == source.name or item in rel for item in filters):
                continue
            cases.append((suite, source))
    if limit > 0:
        cases = cases[:limit]
    return cases


def make_output_dir(requested: Path | None) -> Path:
    if requested is not None:
        output_dir = requested.expanduser().resolve()
    else:
        output_dir = DEFAULT_OUTPUT_ROOT / datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir.mkdir(parents=True, exist_ok=True)
    DEFAULT_OUTPUT_ROOT.mkdir(parents=True, exist_ok=True)
    if requested is None:
        latest = DEFAULT_OUTPUT_ROOT / "latest"
        if latest.exists() or latest.is_symlink():
            latest.unlink()
        latest.symlink_to(output_dir.name)
    return output_dir


def run_command(command: list[str], timeout_sec: int, stdin: bytes | None = None) -> CommandResult:
    started = time.perf_counter()
    try:
        process = subprocess.run(
            command,
            input=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
            check=False,
        )
        return CommandResult(
            process.returncode,
            process.stdout,
            process.stderr,
            time.perf_counter() - started,
        )
    except subprocess.TimeoutExpired as error:
        return CommandResult(
            124,
            error.stdout or b"",
            error.stderr or b"",
            time.perf_counter() - started,
            timed_out=True,
        )


def normalize_text(data: str) -> str:
    lines = [line.rstrip(" \t") for line in data.replace("\r\n", "\n").replace("\r", "\n").split("\n")]
    return "\n".join(lines)


def actual_output(stdout: bytes, exit_code: int) -> str:
    output = normalize_text(stdout.decode("utf-8", errors="replace"))
    if output and not output.endswith("\n"):
        output += "\n"
    return f"{output}{exit_code}\n"


def compare_output(expected_file: Path, stdout: bytes, exit_code: int) -> tuple[str, str, str]:
    if not expected_file.is_file():
        return "NO_EXPECTED", "", actual_output(stdout, exit_code)
    expected = normalize_text(expected_file.read_text(encoding="utf-8", errors="replace"))
    actual = normalize_text(actual_output(stdout, exit_code))
    return ("MATCH" if expected == actual else "MISMATCH", expected, actual)


def parse_total_seconds(stderr: bytes) -> float | None:
    matches = TOTAL_PATTERN.findall(stderr.decode("utf-8", errors="replace"))
    if not matches:
        return None
    hours, minutes, seconds, microseconds = map(int, matches[-1])
    return hours * 3600 + minutes * 60 + seconds + microseconds / 1_000_000


def analyze_assembly(path: Path) -> dict[str, int]:
    metric_names = (
        "asm_instruction_count",
        "asm_load_count",
        "asm_store_count",
        "asm_stack_memory_count",
        "asm_branch_count",
        "asm_call_count",
        "asm_move_count",
        "asm_mul_count",
        "asm_divrem_count",
        "asm_vector_count",
        "asm_max_loop_depth",
        "asm_loop_weighted_instructions",
        "asm_loop_weighted_memory",
        "asm_loop_weighted_branches",
    )
    metrics = {name: 0 for name in metric_names}
    if not path.is_file():
        return metrics

    labels: dict[str, int] = {}
    instructions: list[tuple[str, str]] = []
    for raw_line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line:
            continue
        if line.endswith(":") and not line.startswith((" ", "\t")):
            labels[line[:-1]] = len(instructions)
            continue
        match = ASM_INSTRUCTION_PATTERN.match(line)
        if match is None or match.group(1).startswith("."):
            continue
        instructions.append((match.group(1).lower(), match.group(2)))

    loop_intervals: list[tuple[int, int]] = []
    for index, (mnemonic, operands) in enumerate(instructions):
        if mnemonic not in ASM_BRANCH_MNEMONICS or not operands:
            continue
        target = operands.split(",")[-1].strip()
        target_index = labels.get(target)
        if target_index is not None and target_index <= index:
            loop_intervals.append((target_index, index))

    loop_depths = [0] * len(instructions)
    for begin, end in loop_intervals:
        for index in range(begin, end + 1):
            loop_depths[index] += 1

    metrics["asm_instruction_count"] = len(instructions)
    for index, (mnemonic, operands) in enumerate(instructions):
        is_load = mnemonic in ASM_LOAD_MNEMONICS
        is_store = mnemonic in ASM_STORE_MNEMONICS
        is_branch = mnemonic in ASM_BRANCH_MNEMONICS
        weight = 10 ** min(loop_depths[index], 4)
        metrics["asm_max_loop_depth"] = max(
            metrics["asm_max_loop_depth"], loop_depths[index]
        )
        metrics["asm_loop_weighted_instructions"] += weight
        if is_load:
            metrics["asm_load_count"] += 1
        if is_store:
            metrics["asm_store_count"] += 1
        if (is_load or is_store) and ("(sp)" in operands or "(s0)" in operands):
            metrics["asm_stack_memory_count"] += 1
        if is_load or is_store:
            metrics["asm_loop_weighted_memory"] += weight
        if is_branch:
            metrics["asm_branch_count"] += 1
            metrics["asm_loop_weighted_branches"] += weight
        if mnemonic in {"call", "tail"}:
            metrics["asm_call_count"] += 1
        if mnemonic in {"mv", "fmv.s", "fmv.d"}:
            metrics["asm_move_count"] += 1
        if mnemonic.startswith("mul"):
            metrics["asm_mul_count"] += 1
        if mnemonic.startswith(("div", "rem")):
            metrics["asm_divrem_count"] += 1
        if mnemonic.startswith("v"):
            metrics["asm_vector_count"] += 1
    return metrics


def empty_profile_metrics(args: argparse.Namespace) -> dict[str, object]:
    return {
        "dynamic_status": "SKIP",
        "dynamic_instruction_count": None,
        "dynamic_analysis_wall_sec": None,
        "hot_block_pc": "",
        "hot_block_instruction_count": None,
        "hot_block_execution_count": None,
        "hot_block_dynamic_instruction_count": None,
        "hot_block_dynamic_instruction_share_pct": None,
        "mca_status": "SKIP",
        "mca_model": args.mca_model if args.analysis == "full" else "",
        "mca_iterations": None,
        "mca_instruction_count": None,
        "mca_total_cycles": None,
        "mca_cycles_per_iteration": None,
        "mca_ipc": None,
        "mca_block_rthroughput": None,
        "mca_estimated_hot_cycles": None,
    }


def parse_hotblocks(stderr: bytes) -> list[tuple[int, int, int]]:
    text = stderr.decode("utf-8", errors="replace")
    return [
        (int(pc, 16), int(instructions), int(executions))
        for pc, instructions, executions in HOTBLOCK_PATTERN.findall(text)
    ]


def find_timing_markers(
    *, executable: Path, objdump: str, timeout_sec: int
) -> tuple[list[int], int, CommandResult]:
    result = run_command(
        [objdump, "-d", "--no-show-raw-insn", str(executable)], timeout_sec
    )
    if result.return_code != 0 or result.timed_out:
        return [], 0, result

    stop_pc = 0
    instructions: list[tuple[int, str]] = []
    for raw_line in result.stdout.decode("utf-8", errors="replace").splitlines():
        symbol = re.match(r"^([0-9a-fA-F]+) <_sysy_stoptime>:$", raw_line)
        if symbol is not None:
            stop_pc = int(symbol.group(1), 16)
            continue
        instruction = re.match(r"^\s*([0-9a-fA-F]+):\s+(.+?)\s*$", raw_line)
        if instruction is not None:
            instructions.append((int(instruction.group(1), 16), instruction.group(2)))

    start_pcs: list[int] = []
    for index, (_, instruction) in enumerate(instructions[:-1]):
        if "<_sysy_starttime>" in instruction:
            start_pcs.append(instructions[index + 1][0])
    return sorted(set(start_pcs)), stop_pc, result


def extract_hot_block(
    *,
    executable: Path,
    pc: int,
    instruction_count: int,
    objdump: str,
    output_path: Path,
    timeout_sec: int,
) -> tuple[str, CommandResult]:
    stop_address = pc + instruction_count * 4 + 16
    command = [
        objdump,
        "-d",
        "--no-show-raw-insn",
        f"--start-address=0x{pc:x}",
        f"--stop-address=0x{stop_address:x}",
        str(executable),
    ]
    result = run_command(command, timeout_sec)
    if result.return_code != 0 or result.timed_out:
        return "", result

    instructions: list[str] = []
    started = False
    for raw_line in result.stdout.decode("utf-8", errors="replace").splitlines():
        match = re.match(r"^\s*([0-9a-fA-F]+):\s+(.+?)\s*$", raw_line)
        if match is None:
            continue
        address = int(match.group(1), 16)
        if not started:
            if address != pc:
                continue
            started = True
        instruction = match.group(2).split("#", 1)[0].strip()
        instruction = re.sub(r"\s*<[^>]+>\s*$", "", instruction)
        parts = instruction.split(None, 1)
        if not parts:
            continue
        mnemonic = parts[0].lower()
        operands = parts[1] if len(parts) == 2 else ""
        if mnemonic in ASM_BRANCH_MNEMONICS and operands and mnemonic not in {"ret", "jr"}:
            operand_parts = [part.strip() for part in operands.split(",")]
            operand_parts[-1] = ".Lhot_target"
            operands = ", ".join(operand_parts)
        instructions.append(f"  {mnemonic}{(' ' + operands) if operands else ''}")
        if len(instructions) == instruction_count:
            break

    if len(instructions) != instruction_count:
        return "", result
    assembly = "\n".join(("  .text", ".Lhot_block:", *instructions, ".Lhot_target:", ""))
    output_path.write_text(assembly, encoding="utf-8")
    return assembly, result


def run_mca_analysis(
    *,
    hot_assembly: Path,
    execution_count: int,
    analysis_dir: Path,
    args: argparse.Namespace,
) -> dict[str, object]:
    metrics = empty_profile_metrics(args)
    metrics["dynamic_status"] = "OK"
    command = [
        args.llvm_mca,
        "-mtriple=riscv64-unknown-linux-gnu",
        f"-mcpu={args.mca_model}",
        "-mattr=+64bit,+m,+a,+f,+d,+c",
        f"-iterations={args.mca_iterations}",
        str(hot_assembly),
    ]
    result = run_command(command, args.analysis_timeout)
    (analysis_dir / "mca.stdout").write_bytes(result.stdout)
    (analysis_dir / "mca.stderr").write_bytes(result.stderr)
    if result.timed_out:
        metrics["mca_status"] = "TIMEOUT"
        return metrics
    if result.return_code != 0:
        metrics["mca_status"] = "FAIL"
        return metrics

    text = result.stdout.decode("utf-8", errors="replace")
    parsed: dict[str, int | float] = {}
    for name, pattern in MCA_VALUE_PATTERNS.items():
        match = pattern.search(text)
        if match is None:
            metrics["mca_status"] = "PARSE_FAIL"
            return metrics
        parsed[name] = int(match.group(1)) if name in {
            "mca_iterations", "mca_instruction_count"
        } else float(match.group(1))
    metrics.update(parsed)
    iterations = int(parsed["mca_iterations"])
    total_cycles = float(parsed["mca_total_cycles"])
    cycles_per_iteration = total_cycles / iterations if iterations else 0.0
    metrics["mca_cycles_per_iteration"] = cycles_per_iteration
    metrics["mca_estimated_hot_cycles"] = cycles_per_iteration * execution_count
    metrics["mca_status"] = "OK"
    return metrics


def profile_executable(
    *,
    executable: Path,
    source: Path,
    analysis_dir: Path,
    args: argparse.Namespace,
) -> dict[str, object]:
    metrics = empty_profile_metrics(args)
    if args.analysis == "none":
        return metrics

    analysis_dir.mkdir(parents=True, exist_ok=True)
    start_pcs, stop_pc, marker_result = find_timing_markers(
        executable=executable,
        objdump=args.objdump,
        timeout_sec=args.analysis_timeout,
    )
    (analysis_dir / "executable.objdump").write_bytes(marker_result.stdout)
    (analysis_dir / "objdump.stderr").write_bytes(marker_result.stderr)
    if not start_pcs or stop_pc == 0:
        metrics["dynamic_status"] = "NO_TIMER"
        return metrics

    stdin = source.with_suffix(".in").read_bytes() if source.with_suffix(".in").is_file() else b""
    command = [
        str(args.analysis_runner),
        "-d",
        "plugin",
        "-plugin",
        ",".join(
            (
                f"file={args.timed_profile_plugin}",
                *(f"start=0x{pc:x}" for pc in start_pcs),
                f"stop=0x{stop_pc:x}",
                "limit=128",
            )
        ),
        str(executable),
    ]
    result = run_command(command, args.analysis_timeout, stdin)
    (analysis_dir / "dynamic.stdout").write_bytes(result.stdout)
    (analysis_dir / "dynamic.stderr").write_bytes(result.stderr)
    metrics["dynamic_analysis_wall_sec"] = result.elapsed_sec
    if result.timed_out:
        metrics["dynamic_status"] = "TIMEOUT"
        return metrics
    if result.return_code < 0:
        metrics["dynamic_status"] = "FAIL"
        return metrics

    stderr_text = result.stderr.decode("utf-8", errors="replace")
    instruction_matches = DYNAMIC_INSN_PATTERN.findall(stderr_text)
    hotblocks = parse_hotblocks(result.stderr)
    if not instruction_matches or not hotblocks:
        metrics["dynamic_status"] = "PARSE_FAIL"
        return metrics
    dynamic_count = int(instruction_matches[-1])
    hottest = max(hotblocks, key=lambda item: item[1] * item[2])
    pc, instruction_count, execution_count = hottest
    hot_dynamic_count = instruction_count * execution_count
    metrics.update(
        {
            "dynamic_status": "OK",
            "dynamic_instruction_count": dynamic_count,
            "hot_block_pc": f"0x{pc:016x}",
            "hot_block_instruction_count": instruction_count,
            "hot_block_execution_count": execution_count,
            "hot_block_dynamic_instruction_count": hot_dynamic_count,
            "hot_block_dynamic_instruction_share_pct": (
                hot_dynamic_count / dynamic_count * 100.0 if dynamic_count else 0.0
            ),
        }
    )

    hot_assembly = analysis_dir / "hot-block.s"
    _, objdump_result = extract_hot_block(
        executable=executable,
        pc=pc,
        instruction_count=instruction_count,
        objdump=args.objdump,
        output_path=hot_assembly,
        timeout_sec=args.analysis_timeout,
    )
    (analysis_dir / "hot-block.objdump").write_bytes(objdump_result.stdout)
    (analysis_dir / "hot-block-objdump.stderr").write_bytes(objdump_result.stderr)
    if not hot_assembly.is_file():
        metrics["mca_status"] = "OBJDUMP_FAIL" if args.analysis == "full" else "SKIP"
        return metrics
    if args.analysis != "full":
        return metrics

    mca_metrics = run_mca_analysis(
        hot_assembly=hot_assembly,
        execution_count=execution_count,
        analysis_dir=analysis_dir,
        args=args,
    )
    for name, value in mca_metrics.items():
        if name.startswith("mca_"):
            metrics[name] = value
    return metrics


def save_failure(
    failure_dir: Path,
    source: Path,
    compile_result: CommandResult,
    link_result: CommandResult | None,
    run_result: CommandResult | None,
    expected: str,
    actual: str,
) -> None:
    failure_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, failure_dir / source.name)
    (failure_dir / "compile.stdout").write_bytes(compile_result.stdout)
    (failure_dir / "compile.stderr").write_bytes(compile_result.stderr)
    if link_result is not None:
        (failure_dir / "link.stdout").write_bytes(link_result.stdout)
        (failure_dir / "link.stderr").write_bytes(link_result.stderr)
    if run_result is not None:
        (failure_dir / "run.stdout").write_bytes(run_result.stdout)
        (failure_dir / "run.stderr").write_bytes(run_result.stderr)
    if expected or actual:
        (failure_dir / "expected.out").write_text(expected + "\n", encoding="utf-8")
        (failure_dir / "actual.out").write_text(actual + "\n", encoding="utf-8")


def evaluate_case(
    *,
    channel: str,
    suite: str,
    source: Path,
    compiler: Path,
    runtime_source: Path,
    work_dir: Path,
    args: argparse.Namespace,
) -> CaseResult:
    case_dir = work_dir / channel / suite / source.stem
    case_dir.mkdir(parents=True, exist_ok=True)
    assembly = case_dir / f"{source.stem}.s"
    executable = case_dir / source.stem

    compile_command = [str(compiler), "-S", "-o", str(assembly), str(source)]
    if channel == "performance":
        compile_command.append("-O1")
    compile_result = run_command(compile_command, args.compile_timeout)
    compile_status = (
        "TIMEOUT" if compile_result.timed_out else "OK" if compile_result.return_code == 0 else "FAIL"
    )

    link_result: CommandResult | None = None
    run_result: CommandResult | None = None
    link_status = "SKIP"
    run_status = "SKIP"
    compare_status = "SKIP"
    timer_status = "SKIP"
    timer_sec: float | None = None
    assembly_metrics = analyze_assembly(assembly) if compile_status == "OK" else analyze_assembly(Path())
    profile_metrics = empty_profile_metrics(args)
    expected = ""
    actual = ""

    if compile_status == "OK":
        link_command = [
            args.linker,
            "-static",
            "-march=rv64gc",
            "-mcmodel=medany",
            str(assembly),
            str(runtime_source),
            "-o",
            str(executable),
        ]
        link_result = run_command(link_command, args.link_timeout)
        link_status = (
            "TIMEOUT" if link_result.timed_out else "OK" if link_result.return_code == 0 else "FAIL"
        )

    if link_status == "OK":
        input_file = source.with_suffix(".in")
        stdin = input_file.read_bytes() if input_file.is_file() else b""
        run_result = run_command([args.runner, str(executable)], args.run_timeout, stdin)
        if run_result.timed_out:
            run_status = "TIMEOUT"
        elif run_result.return_code < 0:
            run_status = "FAIL"
        else:
            run_status = "OK"
            if args.verify_output:
                compare_status, expected, actual = compare_output(
                    source.with_suffix(".out"), run_result.stdout, run_result.return_code
                )
            else:
                compare_status = "SKIP"
            if channel == "performance":
                timer_sec = parse_total_seconds(run_result.stderr)
                timer_status = "FOUND" if timer_sec is not None else "MISSING"

    if compile_status == "TIMEOUT":
        final_status = "TLE"
    elif compile_status != "OK":
        final_status = "CE"
    elif link_status == "TIMEOUT":
        final_status = "TLE"
    elif link_status != "OK":
        final_status = "RE"
    elif run_status == "TIMEOUT":
        final_status = "TLE"
    elif run_status != "OK":
        final_status = "RE"
    elif args.verify_output and compare_status != "MATCH":
        final_status = "WA"
    elif channel == "performance" and timer_status != "FOUND":
        final_status = "RE"
    else:
        final_status = "AC"

    if link_status == "OK" and run_status == "OK" and args.analysis != "none":
        profile_metrics = profile_executable(
            executable=executable,
            source=source,
            analysis_dir=work_dir.parent / "analysis" / channel / suite / source.stem,
            args=args,
        )

    link_sec = link_result.elapsed_sec if link_result else 0.0
    run_sec = run_result.elapsed_sec if run_result else 0.0
    result = CaseResult(
        channel=channel,
        suite=suite,
        case=source.stem,
        source=str(source.relative_to(args.bench_root)),
        final_status=final_status,
        compile_status=compile_status,
        compile_exit_code=compile_result.return_code,
        link_status=link_status,
        link_exit_code=link_result.return_code if link_result else -1,
        run_status=run_status,
        run_exit_code=run_result.return_code if run_result else -1,
        compare_status=compare_status,
        timer_status=timer_status,
        compile_sec=compile_result.elapsed_sec,
        link_sec=link_sec,
        run_wall_sec=run_sec,
        timer_sec=timer_sec,
        total_wall_sec=compile_result.elapsed_sec + link_sec + run_sec,
        **assembly_metrics,
        **profile_metrics,
    )

    if final_status != "AC":
        save_failure(
            work_dir / "failures" / channel / suite / source.stem,
            source,
            compile_result,
            link_result,
            run_result,
            expected,
            actual,
        )
    if not args.keep_artifacts:
        shutil.rmtree(case_dir, ignore_errors=True)
    return result


def write_results(output_dir: Path, results: list[CaseResult]) -> None:
    fieldnames = list(CaseResult.__dataclass_fields__)
    with (output_dir / "results.tsv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for result in results:
            writer.writerow(asdict(result))

    status_counts = Counter(result.final_status for result in results)
    channel_counts = Counter((result.channel, result.final_status) for result in results)
    performance_times = [
        result.timer_sec
        for result in results
        if result.channel == "performance" and result.timer_sec is not None
    ]
    summary = {
        "total_cases": len(results),
        "status_counts": dict(sorted(status_counts.items())),
        "channel_status_counts": {
            f"{channel}:{status}": count
            for (channel, status), count in sorted(channel_counts.items())
        },
        "compile_time_sec": sum(result.compile_sec for result in results),
        "link_time_sec": sum(result.link_sec for result in results),
        "run_wall_time_sec": sum(result.run_wall_sec for result in results),
        "total_wall_time_sec": sum(result.total_wall_sec for result in results),
        "performance_timer_sum_sec": sum(performance_times),
        "performance_timer_case_count": len(performance_times),
        "dynamic_profile_case_count": sum(
            result.dynamic_status == "OK" for result in results
        ),
        "dynamic_instruction_total": sum(
            result.dynamic_instruction_count or 0 for result in results
        ),
        "dynamic_analysis_wall_time_sec": sum(
            result.dynamic_analysis_wall_sec or 0.0 for result in results
        ),
        "mca_profile_case_count": sum(result.mca_status == "OK" for result in results),
        "assembly_totals": {
            name: sum(getattr(result, name) for result in results)
            for name in CaseResult.__dataclass_fields__
            if name.startswith("asm_")
        },
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    lines = [
        f"TOTAL={summary['total_cases']}",
        *(f"{status}={count}" for status, count in sorted(status_counts.items())),
        f"compile_time_sec={summary['compile_time_sec']:.6f}",
        f"link_time_sec={summary['link_time_sec']:.6f}",
        f"run_wall_time_sec={summary['run_wall_time_sec']:.6f}",
        f"total_wall_time_sec={summary['total_wall_time_sec']:.6f}",
        f"performance_timer_case_count={summary['performance_timer_case_count']}",
        f"performance_timer_sum_sec={summary['performance_timer_sum_sec']:.6f}",
        f"dynamic_profile_case_count={summary['dynamic_profile_case_count']}",
        f"dynamic_instruction_total={summary['dynamic_instruction_total']}",
        f"dynamic_analysis_wall_time_sec={summary['dynamic_analysis_wall_time_sec']:.6f}",
        f"mca_profile_case_count={summary['mca_profile_case_count']}",
    ]
    (output_dir / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n" + "\n".join(lines))
    print(f"结果目录: {output_dir}")


def write_baseline_comparison(
    output_dir: Path, results: list[CaseResult], requested: Path | None
) -> None:
    if requested is None:
        return
    baseline_path = requested.expanduser().resolve()
    if baseline_path.is_dir():
        baseline_path /= "results.tsv"
    if not baseline_path.is_file():
        raise SystemExit(f"错误: 找不到基线结果 {baseline_path}")

    with baseline_path.open(newline="", encoding="utf-8") as handle:
        baseline_rows = {
            row["source"]: row for row in csv.DictReader(handle, delimiter="\t")
        }
    metric_names = [
        "timer_sec",
        *(
            name
            for name in CaseResult.__dataclass_fields__
            if name.startswith("asm_")
        ),
        *PROFILE_COMPARISON_FIELDS,
    ]
    fieldnames = ["channel", "suite", "case", "source"]
    for name in metric_names:
        fieldnames.extend((f"baseline_{name}", f"current_{name}", f"delta_{name}_pct"))

    with (output_dir / "comparison.tsv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        for result in results:
            baseline = baseline_rows.get(result.source)
            if baseline is None:
                continue
            row: dict[str, object] = {
                "channel": result.channel,
                "suite": result.suite,
                "case": result.case,
                "source": result.source,
            }
            for name in metric_names:
                baseline_text = baseline.get(name, "")
                current_value = getattr(result, name)
                row[f"baseline_{name}"] = baseline_text
                row[f"current_{name}"] = "" if current_value is None else current_value
                try:
                    baseline_value = float(baseline_text)
                    current_number = float(current_value) if current_value is not None else 0.0
                    delta = (
                        (current_number - baseline_value) / baseline_value * 100.0
                        if baseline_value != 0.0
                        else 0.0
                    )
                    row[f"delta_{name}_pct"] = f"{delta:.6f}"
                except (TypeError, ValueError):
                    row[f"delta_{name}_pct"] = ""
            writer.writerow(row)
    print(f"汇编/时间差异: {output_dir / 'comparison.tsv'}")


def main() -> int:
    args = parse_args()
    args.bench_root = args.bench_root.expanduser().resolve()
    args.analysis_runner = args.analysis_runner.expanduser().resolve()
    args.timed_profile_plugin = args.timed_profile_plugin.expanduser().resolve()
    compiler = resolve_compiler(args.compiler)
    filters = split_filters(args.sample)
    output_dir = make_output_dir(args.output_dir)

    if not args.bench_root.is_dir():
        raise SystemExit(f"错误: 找不到 2026 用例根目录 {args.bench_root}")
    for tool in (args.linker, args.runner):
        if shutil.which(tool) is None:
            raise SystemExit(f"错误: 找不到工具 {tool}；请在 compiler-impl2026-dev 容器内运行")
    if args.analysis != "none":
        for path, description in (
            (args.analysis_runner, "启用插件的 QEMU"),
            (args.timed_profile_plugin, "计时区间动态分析插件"),
        ):
            if not path.is_file():
                raise SystemExit(f"错误: 找不到{description} {path}")
        if shutil.which(args.objdump) is None:
            raise SystemExit(f"错误: 找不到反汇编工具 {args.objdump}")
    if args.analysis == "full":
        if args.mca_iterations <= 0:
            raise SystemExit("错误: --mca-iterations 必须大于 0")
        if shutil.which(args.llvm_mca) is None:
            raise SystemExit(f"错误: 找不到 LLVM MCA {args.llvm_mca}")
        cpu_help = run_command(
            [args.llvm_mca, "-mtriple=riscv64-unknown-linux-gnu", "-mcpu=help"], 30
        )
        cpu_text = (cpu_help.stdout + cpu_help.stderr).decode("utf-8", errors="replace")
        if args.mca_model not in cpu_text:
            raise SystemExit(
                f"错误: {args.llvm_mca} 不支持 RISC-V 调度模型 {args.mca_model}"
            )

    runtime_source = output_dir / "runtime" / "sylib.c"
    runtime_source.parent.mkdir(parents=True, exist_ok=True)
    runtime_source.write_text(SYSY_RUNTIME_C, encoding="utf-8")

    channels = ("correctness", "performance") if args.channel == "all" else (args.channel,)
    cases = [
        (channel, suite, source)
        for channel in channels
        for suite, source in discover_cases(args.bench_root, channel, filters, args.limit)
    ]
    if not cases:
        raise SystemExit("错误: 没有匹配的 2026 测试样例")

    results: list[CaseResult] = []
    work_dir = output_dir / "work"
    for index, (channel, suite, source) in enumerate(cases, start=1):
        result = evaluate_case(
            channel=channel,
            suite=suite,
            source=source,
            compiler=compiler,
            runtime_source=runtime_source,
            work_dir=work_dir,
            args=args,
        )
        results.append(result)
        timer = f" timer={result.timer_sec:.6f}s" if result.timer_sec is not None else ""
        dynamic = (
            f" dyn={result.dynamic_instruction_count}"
            if result.dynamic_instruction_count is not None
            else ""
        )
        mca = (
            f" mca={result.mca_cycles_per_iteration:.3f}cy/hot-block"
            if result.mca_cycles_per_iteration is not None
            else ""
        )
        print(
            f"[{index:03d}/{len(cases):03d}] {channel}/{suite}/{source.stem}: "
            f"{result.final_status} compile={result.compile_sec:.3f}s "
            f"link={result.link_sec:.3f}s run={result.run_wall_sec:.3f}s"
            f"{timer}{dynamic}{mca}",
            flush=True,
        )

    write_results(output_dir, results)
    write_baseline_comparison(output_dir, results, args.baseline_results)
    return 0 if all(result.final_status == "AC" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
