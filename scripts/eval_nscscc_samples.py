#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import signal
import subprocess
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Iterable


DEFAULT_COMPILER = Path("/workspace/.docker-build/compiler")
DEFAULT_BENCH_ROOT = Path("/workspace/benchmarks/nscscc-implementation")
DEFAULT_OUTPUT_BASE = Path("/workspace/.nscscc-eval")
DEFAULT_YEARS = ("2023", "2024", "2025")
KNOWN_SUITES = (
    "functional",
    "h_functional",
    "hidden_functional",
    "performance",
    "h_performance",
    "final_performance",
    "riscv_performance",
    "riscv_final_performance",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "只能在容器内运行的 NSCSCC 实现赛样例批量评测脚本。"
            "默认评测 2023/2024/2025，并生成 asm/tokens/ast/ir 调试产物。"
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
        "--timeout",
        type=int,
        default=60,
        help="单个样例超时时间，默认 60 秒",
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
        default="all",
        help="中间产物模式，默认 all",
    )
    return parser.parse_args()


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


def run_command(cmd: list[str], timeout_sec: int) -> tuple[int, bytes, bytes]:
    try:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
            check=False,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        return 124, exc.stdout or b"", exc.stderr or b""


def discover_sources(bench_root: Path, years: Iterable[str]) -> list[Path]:
    sources: list[Path] = []
    for year in years:
        year_root = bench_root / year
        if not year_root.is_dir():
            raise SystemExit(f"错误: 找不到年份目录 {year_root}")
        sources.extend(sorted(year_root.rglob("*.sy")))
        sources.extend(sorted(year_root.rglob("*.sysy")))
    return sorted(set(sources))


def make_output_dir(requested: Path | None) -> Path:
    if requested is not None:
        requested.mkdir(parents=True, exist_ok=True)
        return requested

    run_dir = DEFAULT_OUTPUT_BASE / datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    DEFAULT_OUTPUT_BASE.mkdir(parents=True, exist_ok=True)

    latest_link = DEFAULT_OUTPUT_BASE / "latest"
    if latest_link.exists() or latest_link.is_symlink():
        latest_link.unlink()
    latest_link.symlink_to(run_dir.name)
    return run_dir


def main() -> int:
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

    output_dir = make_output_dir(args.output_dir.resolve() if args.output_dir else None)
    asm_root = output_dir / "asm"
    dumps_root = output_dir / "dumps"
    failures_root = output_dir / "failures"
    tmp_root = output_dir / "tmp"
    for path in (asm_root, dumps_root, failures_root, tmp_root):
        path.mkdir(parents=True, exist_ok=True)

    results_path = output_dir / "results.tsv"
    dumps_path = output_dir / "dumps.tsv"
    summary_path = output_dir / "summary.txt"

    sources = discover_sources(bench_root, years)
    if not sources:
        raise SystemExit("错误: 没有找到任何样例文件。")
    if args.limit > 0:
        sources = sources[: args.limit]

    compile_status_counter: Counter[str] = Counter()
    compile_year_counter: Counter[tuple[str, str]] = Counter()
    compile_suite_counter: Counter[tuple[str, str]] = Counter()
    dump_counter: Counter[tuple[str, str]] = Counter()

    with results_path.open("w", newline="", encoding="utf-8") as results_file, dumps_path.open(
        "w", newline="", encoding="utf-8"
    ) as dumps_file:
        results_writer = csv.writer(results_file, delimiter="\t")
        dumps_writer = csv.writer(dumps_file, delimiter="\t")

        results_writer.writerow(("status", "exit_code", "year", "suite", "source"))
        dumps_writer.writerow(("kind", "status", "exit_code", "source", "artifact"))

        for index, src in enumerate(sources, start=1):
            rel = relative_to_workspace(src)
            year = rel.split("/")[2]
            suite = detect_suite(rel)

            asm_file = asm_root / f"{rel}.s"
            dump_prefix = dumps_root / rel
            failure_prefix = failures_root / rel
            asm_file.parent.mkdir(parents=True, exist_ok=True)
            dump_prefix.parent.mkdir(parents=True, exist_ok=True)
            failure_prefix.parent.mkdir(parents=True, exist_ok=True)

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

            results_writer.writerow((compile_status, compile_code, year, suite, rel))
            compile_status_counter[compile_status] += 1
            compile_year_counter[(year, compile_status)] += 1
            compile_suite_counter[(suite, compile_status)] += 1

            if compile_code != 0:
                if asm_file.exists():
                    asm_file.unlink()
                write_bytes(failure_prefix.with_suffix(".compile.stderr"), compile_stderr)
                if compile_stdout:
                    write_bytes(failure_prefix.with_suffix(".compile.stdout"), compile_stdout)

            need_dump = args.dump_mode == "all" or (
                args.dump_mode == "failures" and compile_code != 0
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

            if index % 100 == 0:
                print(f"processed={index}", flush=True)

    summary_lines = [
        f"评测目录: {output_dir}",
        f"编译器: {compiler}",
        f"目标平台: {args.target}",
        f"年份: {','.join(years)}",
        f"超时(秒): {args.timeout}",
        f"中间产物模式: {args.dump_mode}",
        "",
        "[编译结果汇总]",
        f"TOTAL={sum(compile_status_counter.values())}",
    ]
    for status, count in sorted(compile_status_counter.items()):
        summary_lines.append(f"{status}={count}")

    summary_lines.append("")
    summary_lines.append("[按年份统计]")
    for (year, status), count in sorted(compile_year_counter.items()):
        summary_lines.append(f"{year}\t{status}\t{count}")

    summary_lines.append("")
    summary_lines.append("[按套件统计]")
    for (suite, status), count in sorted(compile_suite_counter.items()):
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
