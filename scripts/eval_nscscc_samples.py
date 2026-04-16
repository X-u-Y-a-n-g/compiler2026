#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import re
import shutil
import signal
import subprocess
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Iterable


DEFAULT_COMPILER = Path("/workspace/.docker-build/compiler")
DEFAULT_BENCH_ROOT = Path("/workspace/benchmarks/nscscc-implementation")
DEFAULT_OUTPUT_BASE = Path("/workspace/.nscscc-eval")
DEFAULT_YEARS = ("2023", "2024", "2025")
STOP_AFTER_CHOICES = ("ir", "asm", "link", "run")
SAMPLE_STAGE_ALIASES = {
    "全部": "all",
    "all": "all",
    "基础": "basic",
    "basic": "basic",
    "进阶": "advanced",
    "advanced": "advanced",
}
SAMPLE_STAGE_LABELS = {
    "all": "全部",
    "basic": "基础",
    "advanced": "进阶",
}
SAMPLE_STAGE_SUITES = {
    "all": frozenset(
        {
            "functional",
            "hidden_functional",
            "h_functional",
            "performance",
            "h_performance",
            "final_performance",
            "riscv_performance",
            "riscv_final_performance",
        }
    ),
    "basic": frozenset({"functional", "hidden_functional", "h_functional"}),
    "advanced": frozenset(
        {"performance", "h_performance", "final_performance", "riscv_performance", "riscv_final_performance"}
    ),
}
SUITE_PRIORITY = {
    "functional": 0,
    "hidden_functional": 1,
    "h_functional": 1,
    "performance": 2,
    "riscv_performance": 2,
    "h_performance": 3,
    "final_performance": 3,
    "riscv_final_performance": 3,
}
KNOWN_SUITES = tuple(SUITE_PRIORITY.keys())
STAGE_PRIORITY = {name: index for index, name in enumerate(STOP_AFTER_CHOICES)}
MAX_NAME_LEN = 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "只能在容器内运行的 NSCSCC 实现赛样例批量评测脚本。"
            "默认按基础到进阶顺序评测 2023/2024/2025 全部样例，"
            "并执行编译、链接、运行与输出对拍。"
        )
    )
    parser.add_argument(
        "--compiler",
        type=Path,
        default=DEFAULT_COMPILER,
        help=f"编译器路径，默认 {DEFAULT_COMPILER}",
    )
    parser.add_argument(
        "--bench-root",
        type=Path,
        default=DEFAULT_BENCH_ROOT,
        help=f"样例根目录，默认 {DEFAULT_BENCH_ROOT}",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="输出目录，默认 /workspace/.nscscc-eval/<timestamp>",
    )
    parser.add_argument(
        "--target",
        default="riscv64",
        help="目标平台，默认 riscv64",
    )
    parser.add_argument(
        "--years",
        default=",".join(DEFAULT_YEARS),
        help="年份列表，逗号分隔，默认 2023,2024,2025",
    )
    parser.add_argument(
        "--stage",
        default="全部",
        help="样例阶段：全部/all、基础/basic、进阶/advanced。默认 全部。",
    )
    parser.add_argument(
        "--sample",
        action="append",
        default=[],
        help="只跑指定样例。可传文件名、相对路径片段，支持重复传入，也支持逗号分隔。",
    )
    parser.add_argument(
        "--stop-after",
        choices=STOP_AFTER_CHOICES,
        default="run",
        help="只评测到指定阶段：ir / asm / link / run，默认 run",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=60,
        help="单个编译/链接步骤超时时间，默认 60 秒",
    )
    parser.add_argument(
        "--run-timeout",
        type=int,
        default=120,
        help="单个运行步骤超时时间，默认 120 秒",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="仅跑前 N 个样例，0 表示不限制",
    )
    parser.add_argument(
        "--dump-mode",
        choices=("all", "failures", "none"),
        default="failures",
        help="附加 dump tokens/ast/ir 的模式，默认 failures",
    )
    parser.add_argument(
        "--verify-output",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="运行阶段是否做标准输出对拍，默认开启",
    )
    parser.add_argument(
        "--runtime-lib",
        type=Path,
        default=None,
        help="显式指定运行时库路径，例如 libsysy_riscv.a",
    )
    parser.add_argument(
        "--linker",
        default=None,
        help="显式指定链接器，默认按 target 自动选择",
    )
    parser.add_argument(
        "--qemu",
        default=None,
        help="显式指定运行器，默认按 target 自动选择",
    )
    parser.add_argument(
        "--text-addr",
        default="0x90000000",
        help="RISC-V 链接脚本文本起始地址，默认 0x90000000",
    )
    return parser.parse_args()


def print_case_status(case_name: str, status: str, final: bool = False) -> None:
    padded = case_name.ljust(MAX_NAME_LEN)
    line = f"\r\033[K{padded}  {status}"
    if final:
        print(line, flush=True)
    else:
        print(line, end="", flush=True)


def ensure_in_container() -> None:
    if Path("/.dockerenv").exists():
        return

    try:
        cgroup = Path("/proc/1/cgroup").read_text(encoding="utf-8", errors="ignore")
    except OSError:
        cgroup = ""

    if any(token in cgroup for token in ("docker", "containerd", "kubepods")):
        return

    raise SystemExit("错误: 该脚本只允许在容器内执行。")


def classify_exit(code: int | None) -> str:
    if code == 0:
        return "OK"
    if code == 124:
        return "TIMEOUT"
    if code in (137, -signal.SIGKILL):
        return "KILLED"
    return "FAIL"


def classify_run_exit(code: int | None) -> str:
    if code == 124:
        return "TIMEOUT"
    if code in (137, -signal.SIGKILL):
        return "KILLED"
    if code is None or code < 0:
        return "FAIL"
    return "OK"


def final_status(
    compile_status: str,
    link_status: str,
    run_status: str,
    compare_status: str,
    stop_after: str,
    verify_output: bool,
) -> str:
    if compile_status != "OK":
        return f"COMPILE_{compile_status}"
    if STAGE_PRIORITY[stop_after] <= STAGE_PRIORITY["asm"]:
        return "OK"
    if link_status != "OK":
        return f"LINK_{link_status}"
    if stop_after == "link":
        return "OK"
    if run_status != "OK":
        return f"RUN_{run_status}"
    if verify_output and compare_status != "MATCH":
        return compare_status
    return "OK"


def detect_suite(rel_path: str) -> str:
    for suite in KNOWN_SUITES:
        if f"/{suite}/" in rel_path:
            return suite
    return "unknown"


def relative_to_workspace(path: Path) -> str:
    try:
        return str(path.relative_to("/workspace"))
    except ValueError:
        return str(path)


def write_bytes(path: Path, data: bytes | None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data or b"")


def run_command(
    cmd: list[str], timeout_sec: int, stdin_data: bytes | None = None
) -> tuple[int, bytes, bytes]:
    try:
        proc = subprocess.run(
            cmd,
            input=stdin_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
            check=False,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        return 124, exc.stdout or b"", exc.stderr or b""


def split_natural(text: str) -> list[tuple[int, object]]:
    parts = re.split(r"(\d+)", text)
    key: list[tuple[int, object]] = []
    for part in parts:
        if not part:
            continue
        if part.isdigit():
            key.append((0, int(part)))
        else:
            key.append((1, part.lower()))
    return key


def normalize_sample_filters(filters: Iterable[str]) -> list[str]:
    normalized: list[str] = []
    for item in filters:
        for part in item.split(","):
            part = part.strip()
            if part:
                normalized.append(part)
    return normalized


def normalize_sample_stage(stage: str) -> str:
    normalized = SAMPLE_STAGE_ALIASES.get(stage.strip().lower())
    if normalized is None:
        normalized = SAMPLE_STAGE_ALIASES.get(stage.strip())
    if normalized is None:
        choices = "、".join(("全部/all", "基础/basic", "进阶/advanced"))
        raise SystemExit(f"错误: 不支持的样例阶段 {stage!r}，可选值为 {choices}")
    return normalized


def sample_stage_label(stage: str) -> str:
    return SAMPLE_STAGE_LABELS.get(stage, stage)


def sample_matches(path: Path, filters: list[str]) -> bool:
    if not filters:
        return True
    rel = relative_to_workspace(path)
    basename = path.name
    for pattern in filters:
        if pattern in rel or pattern == basename:
            return True
    return False


def suite_matches_stage(path: Path, stage: str) -> bool:
    suite = detect_suite(relative_to_workspace(path))
    return suite in SAMPLE_STAGE_SUITES[stage]


def source_sort_key(year: str, path: Path) -> tuple[object, ...]:
    rel_from_year = path.relative_to(DEFAULT_BENCH_ROOT / year)
    rel_text = "/" + str(rel_from_year).replace("\\", "/")
    suite = detect_suite(rel_text)
    suite_rank = SUITE_PRIORITY.get(suite, 99)
    return (
        suite_rank,
        split_natural(str(rel_from_year.parent)),
        split_natural(path.stem),
        split_natural(path.suffix),
    )


def discover_sources(
    bench_root: Path, years: Iterable[str], sample_filters: list[str], stage: str
) -> list[Path]:
    sources: list[Path] = []
    seen: set[Path] = set()
    for year in years:
        year_root = bench_root / year
        if not year_root.is_dir():
            raise SystemExit(f"错误: 找不到年份目录 {year_root}")

        year_sources = sorted(
            [*year_root.rglob("*.sy"), *year_root.rglob("*.sysy")],
            key=lambda path: source_sort_key(year, path),
        )
        for source in year_sources:
            if source in seen:
                continue
            if not suite_matches_stage(source, stage):
                continue
            if not sample_matches(source, sample_filters):
                continue
            seen.add(source)
            sources.append(source)
    return sources


def make_output_dir(requested: Path | None) -> Path:
    if requested is not None:
        requested.mkdir(parents=True, exist_ok=True)
        return requested

    run_dir = DEFAULT_OUTPUT_BASE / datetime.now().strftime("%Y%m%d-%H%M%S")
    DEFAULT_OUTPUT_BASE.mkdir(parents=True, exist_ok=True)
    run_dir.mkdir(parents=True, exist_ok=True)

    latest_link = DEFAULT_OUTPUT_BASE / "latest"
    if latest_link.exists() or latest_link.is_symlink():
        latest_link.unlink(missing_ok=True)
    latest_link.symlink_to(run_dir.name)
    return run_dir


def target_tools(target: str) -> tuple[str, str, str]:
    if target == "riscv64":
        return "riscv64-linux-gnu-gcc", "qemu-riscv64", "libsysy_riscv.a"
    raise SystemExit(f"错误: 暂不支持 target={target} 的运行评测。")


def resolve_runtime_lib(bench_root: Path, year: str, target: str, override: Path | None) -> Path:
    if override is not None:
        runtime_lib = override.resolve()
        if not runtime_lib.is_file():
            raise SystemExit(f"错误: 指定的 runtime 库不存在 {runtime_lib}")
        return runtime_lib

    _, _, runtime_name = target_tools(target)
    candidates = [
        bench_root / year / "runtime" / runtime_name,
        bench_root / "2025" / "runtime" / runtime_name,
        bench_root / "2024" / "runtime" / runtime_name,
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit(f"错误: 找不到 {target} 对应的 runtime 库 {runtime_name}")


def normalize_text(text: str, *, strip_trailing_blank_lines: bool) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    lines = [line.rstrip(" \t") for line in text.split("\n")]
    if strip_trailing_blank_lines:
        while lines and lines[-1] == "":
            lines.pop()
    if not lines:
        return ""
    joined = "\n".join(lines)
    if text.endswith("\n") and not joined.endswith("\n"):
        joined += "\n"
    return joined


def build_actual_output(stdout: bytes, exit_code: int) -> str:
    stdout_text = normalize_text(
        stdout.decode("utf-8", errors="ignore"),
        strip_trailing_blank_lines=False,
    )
    if not stdout_text:
        return f"{exit_code}\n"
    if stdout_text.endswith("\n"):
        return f"{stdout_text}{exit_code}\n"
    return f"{stdout_text}\n{exit_code}\n"


def compare_output(expected_path: Path, stdout: bytes, exit_code: int) -> tuple[str, str, str]:
    expected_text = normalize_text(
        expected_path.read_text(encoding="utf-8", errors="ignore"),
        strip_trailing_blank_lines=False,
    )
    actual_text = build_actual_output(stdout, exit_code)
    status = "MATCH" if expected_text == actual_text else "MISMATCH"
    return status, expected_text, actual_text


def stage_compile_status(stop_after: str) -> str:
    if stop_after == "ir":
        return "Compiling sy to ir"
    return "Compiling sy to asm"


def main() -> int:
    global MAX_NAME_LEN
    args = parse_args()
    ensure_in_container()

    compiler = args.compiler.resolve()
    bench_root = args.bench_root.resolve()
    if not compiler.is_file():
        raise SystemExit(f"错误: 找不到可执行编译器 {compiler}")
    if not os.access(compiler, os.X_OK):
        raise SystemExit(f"错误: 编译器不可执行 {compiler}")
    if not bench_root.is_dir():
        raise SystemExit(f"错误: 找不到样例目录 {bench_root}")

    years = [year.strip() for year in args.years.split(",") if year.strip()]
    if not years:
        raise SystemExit("错误: 至少需要一个年份。")

    sample_stage = normalize_sample_stage(args.stage)
    sample_filters = normalize_sample_filters(args.sample)
    verify_output = args.stop_after == "run" and args.verify_output
    need_link = STAGE_PRIORITY[args.stop_after] >= STAGE_PRIORITY["link"]
    need_run = args.stop_after == "run"

    linker, qemu, _ = target_tools(args.target)
    linker = args.linker or linker
    qemu = args.qemu or qemu
    if need_link:
        if shutil.which(linker) is None:
            raise SystemExit(f"错误: 找不到链接器 {linker}")
    if need_run:
        if shutil.which(qemu) is None:
            raise SystemExit(f"错误: 找不到运行器 {qemu}")

    output_dir = make_output_dir(args.output_dir.resolve() if args.output_dir else None)
    ir_root = output_dir / "ir"
    asm_root = output_dir / "asm"
    bin_root = output_dir / "bin"
    stdout_root = output_dir / "stdout"
    dumps_root = output_dir / "dumps"
    failures_root = output_dir / "failures"
    for path in (ir_root, asm_root, bin_root, stdout_root, dumps_root, failures_root):
        path.mkdir(parents=True, exist_ok=True)

    results_path = output_dir / "results.tsv"
    dumps_path = output_dir / "dumps.tsv"
    summary_path = output_dir / "summary.txt"

    sources = discover_sources(bench_root, years, sample_filters, sample_stage)
    if not sources:
        raise SystemExit("错误: 没有找到任何样例文件。")
    if args.limit > 0:
        sources = sources[: args.limit]
    MAX_NAME_LEN = max(len(src.name) for src in sources)

    compile_status_counter: Counter[str] = Counter()
    link_status_counter: Counter[str] = Counter()
    run_status_counter: Counter[str] = Counter()
    compare_status_counter: Counter[str] = Counter()
    final_status_counter: Counter[str] = Counter()
    final_year_counter: Counter[tuple[str, str]] = Counter()
    final_suite_counter: Counter[tuple[str, str]] = Counter()
    dump_counter: Counter[tuple[str, str]] = Counter()

    with results_path.open("w", newline="", encoding="utf-8") as results_file, dumps_path.open(
        "w", newline="", encoding="utf-8"
    ) as dumps_file:
        results_writer = csv.writer(results_file, delimiter="\t")
        dumps_writer = csv.writer(dumps_file, delimiter="\t")

        results_writer.writerow(
            (
                "final_status",
                "compile_status",
                "compile_exit_code",
                "link_status",
                "link_exit_code",
                "run_status",
                "run_exit_code",
                "compare_status",
                "year",
                "suite",
                "source",
            )
        )
        dumps_writer.writerow(("kind", "status", "exit_code", "source", "artifact"))

        for index, src in enumerate(sources, start=1):
            rel = relative_to_workspace(src)
            year = rel.split("/")[2]
            suite = detect_suite(rel)
            case_name = src.name
            print_case_status(case_name, f"[{index}/{len(sources)}] {stage_compile_status(args.stop_after)}")

            ir_file = ir_root / f"{rel}.ll"
            asm_file = asm_root / f"{rel}.s"
            bin_file = bin_root / rel
            stdout_file = stdout_root / f"{rel}.stdout"
            stderr_file = stdout_root / f"{rel}.stderr"
            actual_out_file = stdout_root / f"{rel}.actual.out"
            dump_prefix = dumps_root / rel
            failure_prefix = failures_root / rel
            for path in (
                ir_file.parent,
                asm_file.parent,
                bin_file.parent,
                stdout_file.parent,
                dump_prefix.parent,
                failure_prefix.parent,
            ):
                path.mkdir(parents=True, exist_ok=True)

            if args.stop_after == "ir":
                compile_cmd = [str(compiler), str(src), "--dump-ir"]
            else:
                compile_cmd = [
                    str(compiler),
                    str(src),
                    "--target",
                    args.target,
                    "-o",
                    str(asm_file),
                ]

            compile_code, compile_stdout, compile_stderr = run_command(compile_cmd, args.timeout)
            compile_status = classify_exit(compile_code)
            compile_status_counter[compile_status] += 1

            link_status = "SKIP"
            link_code = -1
            run_status = "SKIP"
            run_code = -1
            compare_status = "SKIP"
            compare_expected = ""
            compare_actual = ""

            if compile_status == "OK" and args.stop_after == "ir":
                write_bytes(ir_file, compile_stdout)
            elif compile_code != 0:
                if asm_file.exists():
                    asm_file.unlink()
                if ir_file.exists():
                    ir_file.unlink()
                write_bytes(failure_prefix.with_suffix(".compile.stderr"), compile_stderr)
                if compile_stdout:
                    write_bytes(failure_prefix.with_suffix(".compile.stdout"), compile_stdout)

            if need_link and compile_status == "OK":
                runtime_lib = resolve_runtime_lib(bench_root, year, args.target, args.runtime_lib)
                print_case_status(case_name, f"[{index}/{len(sources)}] Linking asm to exec")
                link_cmd = [
                    linker,
                    str(asm_file),
                    str(runtime_lib),
                    "-o",
                    str(bin_file),
                ]
                if args.target == "riscv64" and "unknown-elf" in Path(linker).name:
                    link_cmd.extend(
                        [
                            "-static",
                            "-mcmodel=medany",
                            f"-Wl,--no-relax,-Ttext={args.text_addr}",
                        ]
                    )
                else:
                    link_cmd.append("-static")
                link_code, link_stdout, link_stderr = run_command(link_cmd, args.timeout)
                link_status = classify_exit(link_code)
                link_status_counter[link_status] += 1

                if link_code != 0:
                    if bin_file.exists():
                        bin_file.unlink()
                    write_bytes(failure_prefix.with_suffix(".link.stderr"), link_stderr)
                    if link_stdout:
                        write_bytes(failure_prefix.with_suffix(".link.stdout"), link_stdout)

            if need_run and link_status == "OK":
                input_file = src.with_suffix(".in")
                expected_file = src.with_suffix(".out")
                stdin_data = input_file.read_bytes() if input_file.exists() else b""
                print_case_status(case_name, f"[{index}/{len(sources)}] Executing")
                run_cmd = [qemu, str(bin_file)]
                run_code, run_stdout, run_stderr = run_command(
                    run_cmd, args.run_timeout, stdin_data=stdin_data
                )
                run_status = classify_run_exit(run_code)
                run_status_counter[run_status] += 1
                write_bytes(stdout_file, run_stdout)
                write_bytes(stderr_file, run_stderr)

                if run_status != "OK":
                    write_bytes(failure_prefix.with_suffix(".run.stdout"), run_stdout)
                    write_bytes(failure_prefix.with_suffix(".run.stderr"), run_stderr)
                elif verify_output:
                    compare_status, compare_expected, compare_actual = compare_output(
                        expected_file, run_stdout, run_code
                    )
                    compare_status_counter[compare_status] += 1
                    write_bytes(actual_out_file, compare_actual.encode("utf-8"))
                    if compare_status != "MATCH":
                        write_bytes(
                            failure_prefix.with_suffix(".expected.out"),
                            compare_expected.encode("utf-8"),
                        )
                        write_bytes(
                            failure_prefix.with_suffix(".actual.out"),
                            compare_actual.encode("utf-8"),
                        )
                        write_bytes(failure_prefix.with_suffix(".run.stdout"), run_stdout)
                        write_bytes(failure_prefix.with_suffix(".run.stderr"), run_stderr)

            row_final_status = final_status(
                compile_status,
                link_status,
                run_status,
                compare_status,
                args.stop_after,
                verify_output,
            )
            final_status_counter[row_final_status] += 1
            final_year_counter[(year, row_final_status)] += 1
            final_suite_counter[(suite, row_final_status)] += 1

            if row_final_status == "OK":
                final_label = "\033[92mAccepted\033[0m"
            elif row_final_status == "MISMATCH":
                final_label = "\033[91mWrong Answer\033[0m"
            elif row_final_status.startswith("COMPILE_"):
                final_label = "\033[93mCompiler Error\033[0m"
            elif row_final_status.startswith("LINK_"):
                final_label = "\033[93mLink Error\033[0m"
            elif row_final_status.startswith("RUN_TIMEOUT"):
                final_label = "\033[93mExecute Time Limit Exceed\033[0m"
            elif row_final_status.startswith("RUN_KILLED") or row_final_status.startswith("RUN_FAIL"):
                final_label = "\033[93mRuntime Error\033[0m"
            else:
                final_label = row_final_status
            print_case_status(case_name, final_label, final=True)

            results_writer.writerow(
                (
                    row_final_status,
                    compile_status,
                    compile_code,
                    link_status,
                    link_code,
                    run_status,
                    run_code,
                    compare_status,
                    year,
                    suite,
                    rel,
                )
            )

            need_dump = args.dump_mode == "all" or (
                args.dump_mode == "failures" and row_final_status != "OK"
            )
            if need_dump:
                for dump_name in ("tokens", "ast", "ir"):
                    dump_file = dump_prefix.with_suffix(f"{src.suffix}.{dump_name}.txt")
                    dump_stderr_path = dump_prefix.with_suffix(f"{src.suffix}.{dump_name}.stderr")
                    dump_cmd = [
                        str(compiler),
                        str(src),
                        "--target",
                        args.target,
                        f"--dump-{dump_name}",
                    ]
                    dump_code, dump_stdout, dump_stderr = run_command(dump_cmd, args.timeout)
                    dump_status = classify_exit(dump_code)
                    write_bytes(dump_file, dump_stdout)
                    write_bytes(dump_stderr_path, dump_stderr)
                    dumps_writer.writerow(
                        (
                            dump_name,
                            dump_status,
                            dump_code,
                            rel,
                            relative_to_workspace(dump_file),
                        )
                    )
                    dump_counter[(dump_name, dump_status)] += 1

    summary_lines = [
        f"评测目录: {output_dir}",
        f"编译器: {compiler}",
        f"目标平台: {args.target}",
        f"年份: {','.join(years)}",
        f"样例阶段: {sample_stage_label(sample_stage)}",
        f"样例过滤: {', '.join(sample_filters) if sample_filters else '全部'}",
        f"终止阶段: {args.stop_after}",
        f"编译/链接超时(秒): {args.timeout}",
        f"运行超时(秒): {args.run_timeout}",
        f"输出对拍: {'开启' if verify_output else '关闭'}",
        f"中间产物模式: {args.dump_mode}",
        "",
        "[最终结果汇总]",
        f"TOTAL={sum(final_status_counter.values())}",
    ]
    for status, count in sorted(final_status_counter.items()):
        summary_lines.append(f"{status}={count}")

    summary_lines.append("")
    summary_lines.append("[编译结果汇总]")
    for status, count in sorted(compile_status_counter.items()):
        summary_lines.append(f"{status}={count}")

    if need_link:
        summary_lines.append("")
        summary_lines.append("[链接结果汇总]")
        for status, count in sorted(link_status_counter.items()):
            summary_lines.append(f"{status}={count}")

    if need_run:
        summary_lines.append("")
        summary_lines.append("[运行结果汇总]")
        for status, count in sorted(run_status_counter.items()):
            summary_lines.append(f"{status}={count}")

    if verify_output:
        summary_lines.append("")
        summary_lines.append("[输出对拍汇总]")
        for status, count in sorted(compare_status_counter.items()):
            summary_lines.append(f"{status}={count}")

    summary_lines.append("")
    summary_lines.append("[按年份统计]")
    for (year, status), count in sorted(final_year_counter.items()):
        summary_lines.append(f"{year}\t{status}\t{count}")

    summary_lines.append("")
    summary_lines.append("[按套件统计]")
    for (suite, status), count in sorted(final_suite_counter.items()):
        summary_lines.append(f"{suite}\t{status}\t{count}")

    summary_lines.append("")
    summary_lines.append("[中间产物汇总]")
    if dump_counter:
        for (kind, status), count in sorted(dump_counter.items()):
            summary_lines.append(f"{kind}\t{status}\t{count}")
    else:
        summary_lines.append("无")

    summary_text = "\n".join(summary_lines) + "\n"
    summary_path.write_text(summary_text, encoding="utf-8")
    print(summary_text, end="")
    print(f"明细结果: {relative_to_workspace(results_path)}")
    print(f"Dump 明细: {relative_to_workspace(dumps_path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
