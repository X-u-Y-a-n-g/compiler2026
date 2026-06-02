#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import os
import re
import shlex
import shutil
import signal
import statistics
import subprocess
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Iterable


DEFAULT_COMPILER = Path("/workspace/build/compiler")
DEFAULT_BENCH_ROOT = Path("/workspace/benchmarks/nscscc-implementation/2026")
DEFAULT_OUTPUT_BASE = Path("/workspace/.nscscc-eval-2026")
SUITES = ("functional", "h_functional", "performance")
SUITE_ORDER = {suite: index for index, suite in enumerate(SUITES)}
OPT_POLICIES = ("suite", "always-o0", "always-o1")


@dataclass(frozen=True)
class CaseResult:
    suite: str
    source: str
    opt_level: str
    final_status: str
    compile_status: str
    compile_exit_code: int
    link_status: str
    link_exit_code: int
    run_status: str
    run_exit_code: int
    compare_status: str
    compile_sec: float
    link_sec: float
    run_sec: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "2026 NSCSCC RISC-V 本地自测脚本：只评测 benchmarks/nscscc-implementation/2026，"
            "调用本项目 compiler 生成 .s，再用 riscv64-linux-gnu-gcc 链接 runtime，"
            "最后通过 qemu-riscv64 或自定义 runner 运行并对拍 .out。"
        )
    )
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER, help=f"编译器路径，默认 {DEFAULT_COMPILER}")
    parser.add_argument("--bench-root", type=Path, default=DEFAULT_BENCH_ROOT, help=f"2026 样例目录，默认 {DEFAULT_BENCH_ROOT}")
    parser.add_argument("--output-dir", type=Path, default=None, help="输出目录，默认 /workspace/.nscscc-eval-2026/<timestamp>")
    parser.add_argument(
        "--suite",
        action="append",
        default=[],
        help="选择样例目录：functional、h_functional、performance、all。可重复传入，也可逗号分隔。默认 all。",
    )
    parser.add_argument("--sample", action="append", default=[], help="按文件名或相对路径片段筛选样例，可重复传入，也可逗号分隔。")
    parser.add_argument("--limit", type=int, default=0, help="只跑前 N 个样例，0 表示不限制。")
    parser.add_argument("--runtime-lib", type=Path, default=None, help="显式指定 libsysy_riscv.a。默认优先 2026/runtime，再回退 2025/runtime。")
    parser.add_argument("--linker", default="riscv64-linux-gnu-gcc", help="RISC-V 链接器，默认 riscv64-linux-gnu-gcc。")
    parser.add_argument(
        "--runner-command",
        default="qemu-riscv64 {exe}",
        help='运行命令模板，必须包含 {exe}；默认 "qemu-riscv64 {exe}"。可改成硬件 runner/ssh 脚本。',
    )
    parser.add_argument("--timeout", type=int, default=60, help="单个编译/链接步骤超时秒数，默认 60。")
    parser.add_argument("--run-timeout", type=int, default=120, help="单次执行超时秒数，默认 120。")
    parser.add_argument("--runs", type=int, default=1, help="每个样例正式运行次数，默认 1；performance 可设为 3/5 做统计。")
    parser.add_argument("--warmup-runs", type=int, default=0, help="正式计时前预热次数，默认 0。")
    parser.add_argument(
        "--opt-policy",
        choices=OPT_POLICIES,
        default="suite",
        help="优化等级策略：suite 表示 functional/h_functional 用 -O0、performance 用 -O1；也可强制 always-o0/always-o1。",
    )
    parser.add_argument("--keep-going", action=argparse.BooleanOptionalAction, default=True, help="遇到失败后是否继续，默认继续。")
    parser.add_argument(
        "--verify-output",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="是否对拍 .out，默认开启。",
    )
    return parser.parse_args()


def split_natural(text: str) -> list[tuple[int, object]]:
    parts = re.split(r"(\d+)", text)
    key: list[tuple[int, object]] = []
    for part in parts:
        if not part:
            continue
        key.append((0, int(part)) if part.isdigit() else (1, part.lower()))
    return key


def split_csv(items: Iterable[str]) -> list[str]:
    result: list[str] = []
    for item in items:
        for part in item.split(","):
            stripped = part.strip()
            if stripped:
                result.append(stripped)
    return result


def normalize_suites(raw_suites: Iterable[str]) -> set[str]:
    requested = split_csv(raw_suites)
    if not requested or "all" in requested:
        return set(SUITES)
    invalid = sorted(set(requested) - set(SUITES))
    if invalid:
        raise SystemExit(f"错误: 不支持的 suite: {', '.join(invalid)}；可选值为 {', '.join(SUITES)} 或 all")
    return set(requested)


def sample_matches(path: Path, bench_root: Path, filters: list[str]) -> bool:
    if not filters:
        return True
    rel = str(path.relative_to(bench_root)).replace("\\", "/")
    return any(pattern == path.name or pattern in rel for pattern in filters)


def discover_sources(bench_root: Path, suites: set[str], sample_filters: list[str]) -> list[Path]:
    sources: list[Path] = []
    for suite in SUITES:
        suite_dir = bench_root / suite
        if suite not in suites:
            continue
        if not suite_dir.is_dir():
            raise SystemExit(f"错误: 找不到样例目录 {suite_dir}")
        suite_sources = sorted(suite_dir.glob("*.sy"), key=lambda path: split_natural(path.name))
        sources.extend(path for path in suite_sources if sample_matches(path, bench_root, sample_filters))
    return sources


def make_output_dir(requested: Path | None) -> Path:
    if requested is not None:
        requested.mkdir(parents=True, exist_ok=True)
        return requested
    DEFAULT_OUTPUT_BASE.mkdir(parents=True, exist_ok=True)
    run_dir = DEFAULT_OUTPUT_BASE / datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    latest_link = DEFAULT_OUTPUT_BASE / "latest"
    if latest_link.exists() or latest_link.is_symlink():
        latest_link.unlink()
    latest_link.symlink_to(run_dir.name)
    return run_dir


def resolve_runtime_lib(bench_root: Path, override: Path | None) -> Path:
    candidates = []
    if override is not None:
        candidates.append(override)
    candidates.extend(
        [
            bench_root / "runtime" / "libsysy_riscv.a",
            bench_root.parent / "2025" / "runtime" / "libsysy_riscv.a",
        ]
    )
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_file():
            return resolved
    searched = "\n  ".join(str(path) for path in candidates)
    raise SystemExit(f"错误: 找不到 RISC-V runtime 库，已搜索:\n  {searched}")


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
    stdout_text = normalize_text(stdout.decode("utf-8", errors="ignore"), strip_trailing_blank_lines=False)
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


def run_command(cmd: list[str], timeout_sec: int, stdin_data: bytes | None = None) -> tuple[int, bytes, bytes, float]:
    start = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            input=stdin_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_sec,
            check=False,
        )
        elapsed = time.perf_counter() - start
        return proc.returncode, proc.stdout, proc.stderr, elapsed
    except subprocess.TimeoutExpired as exc:
        elapsed = time.perf_counter() - start
        return 124, exc.stdout or b"", exc.stderr or b"", elapsed


def runner_command(template: str, exe: Path) -> list[str]:
    if "{exe}" not in template:
        raise SystemExit("错误: --runner-command 必须包含 {exe} 占位符。")
    return [part.replace("{exe}", str(exe)) for part in shlex.split(template)]


def require_tool(command: str, label: str) -> None:
    executable = shlex.split(command)[0] if " " in command else command
    if os.path.isabs(executable) or "/" in executable:
        if not Path(executable).exists():
            raise SystemExit(f"错误: 找不到{label} {executable}")
        return
    if shutil.which(executable) is None:
        raise SystemExit(f"错误: PATH 中找不到{label} {executable}")


def opt_level_for(suite: str, policy: str) -> str:
    if policy == "always-o0":
        return "-O0"
    if policy == "always-o1":
        return "-O1"
    return "-O1" if suite == "performance" else "-O0"


def final_status(compile_status: str, link_status: str, run_status: str, compare_status: str, verify_output: bool) -> str:
    if compile_status != "OK":
        return f"COMPILE_{compile_status}"
    if link_status != "OK":
        return f"LINK_{link_status}"
    if run_status != "OK":
        return f"RUN_{run_status}"
    if verify_output and compare_status != "MATCH":
        return compare_status
    return "OK"


def write_bytes(path: Path, data: bytes | None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data or b"")


def case_artifact(output_root: Path, src: Path, bench_root: Path, suffix: str) -> Path:
    rel = src.relative_to(bench_root)
    return output_root / rel.with_suffix(suffix)


def print_progress(index: int, total: int, src: Path, message: str, *, final: bool = False) -> None:
    line = f"\r\033[K[{index}/{total}] {src.parent.name}/{src.name}  {message}"
    if final:
        print(line, flush=True)
    else:
        print(line, end="", flush=True)


def write_summary(output_dir: Path, results: list[CaseResult], time_rows: list[dict[str, object]]) -> None:
    status_counter = Counter(result.final_status for result in results)
    suite_counter = Counter((result.suite, result.final_status) for result in results)
    ok_times_by_suite: dict[str, list[float]] = defaultdict(list)
    for row in time_rows:
        if row["kind"] == "measure" and row["status"] == "OK":
            ok_times_by_suite[str(row["suite"])].append(float(row["elapsed_sec"]))

    lines = [
        f"TOTAL={len(results)}",
        " ".join(f"{status}={count}" for status, count in sorted(status_counter.items())),
        "",
        "By suite:",
    ]
    for suite in SUITES:
        suite_total = sum(count for (name, _), count in suite_counter.items() if name == suite)
        parts = [f"{status}={suite_counter[(suite, status)]}" for status in sorted(status_counter) if suite_counter[(suite, status)]]
        lines.append(f"  {suite}: TOTAL={suite_total} {' '.join(parts)}".rstrip())
    lines.append("")
    lines.append("Performance timing:")
    for suite in SUITES:
        values = ok_times_by_suite.get(suite, [])
        if not values:
            lines.append(f"  {suite}: no successful measured runs")
            continue
        lines.append(
            f"  {suite}: runs={len(values)} mean={statistics.fmean(values):.6f}s "
            f"median={statistics.median(values):.6f}s min={min(values):.6f}s max={max(values):.6f}s"
        )
    (output_dir / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")

    report_lines = [
        "# 2026 NSCSCC RISC-V Evaluation Report",
        "",
        f"- Total cases: {len(results)}",
        *[f"- {status}: {count}" for status, count in sorted(status_counter.items())],
        "",
        "## Suites",
        "",
        "| Suite | Total | OK | Other |",
        "| --- | ---: | ---: | ---: |",
    ]
    for suite in SUITES:
        suite_total = sum(count for (name, _), count in suite_counter.items() if name == suite)
        ok_count = suite_counter[(suite, "OK")]
        report_lines.append(f"| {suite} | {suite_total} | {ok_count} | {suite_total - ok_count} |")
    report_lines.extend(["", "## Timing", "", "| Suite | Runs | Mean(s) | Median(s) | Min(s) | Max(s) |", "| --- | ---: | ---: | ---: | ---: | ---: |"])
    for suite in SUITES:
        values = ok_times_by_suite.get(suite, [])
        if values:
            report_lines.append(
                f"| {suite} | {len(values)} | {statistics.fmean(values):.6f} | {statistics.median(values):.6f} | {min(values):.6f} | {max(values):.6f} |"
            )
        else:
            report_lines.append(f"| {suite} | 0 | - | - | - | - |")
    failures = [result for result in results if result.final_status != "OK"]
    if failures:
        report_lines.extend(["", "## Failures", "", "| Status | Suite | Source |", "| --- | --- | --- |"])
        for result in failures[:100]:
            report_lines.append(f"| {result.final_status} | {result.suite} | {result.source} |")
        if len(failures) > 100:
            report_lines.append(f"| ... | ... | {len(failures) - 100} more |")
    (output_dir / "report.md").write_text("\n".join(report_lines) + "\n", encoding="utf-8")


def main() -> int:
    args = parse_args()
    compiler = args.compiler.resolve()
    bench_root = args.bench_root.resolve()
    if not compiler.is_file():
        raise SystemExit(f"错误: 找不到编译器 {compiler}")
    if not os.access(compiler, os.X_OK):
        raise SystemExit(f"错误: 编译器不可执行 {compiler}")
    if not bench_root.is_dir():
        raise SystemExit(f"错误: 找不到 2026 样例目录 {bench_root}")
    if args.runs <= 0:
        raise SystemExit("错误: --runs 必须大于 0")
    if args.warmup_runs < 0:
        raise SystemExit("错误: --warmup-runs 不能小于 0")

    suites = normalize_suites(args.suite)
    sample_filters = split_csv(args.sample)
    sources = discover_sources(bench_root, suites, sample_filters)
    if args.limit > 0:
        sources = sources[: args.limit]
    if not sources:
        raise SystemExit("错误: 没有找到任何 2026 样例。")

    require_tool(args.linker, "链接器")
    require_tool(args.runner_command.format(exe="__placeholder__"), "运行器")
    runtime_lib = resolve_runtime_lib(bench_root, args.runtime_lib)
    output_dir = make_output_dir(args.output_dir.resolve() if args.output_dir else None)

    asm_root = output_dir / "asm"
    bin_root = output_dir / "bin"
    stdout_root = output_dir / "stdout"
    failures_root = output_dir / "failures"
    for path in (asm_root, bin_root, stdout_root, failures_root):
        path.mkdir(parents=True, exist_ok=True)

    results_path = output_dir / "results.tsv"
    times_path = output_dir / "times.tsv"
    perf_path = output_dir / "performance.tsv"
    results: list[CaseResult] = []
    time_rows: list[dict[str, object]] = []
    perf_rows: list[dict[str, object]] = []

    print(f"Benchmark root: {bench_root}")
    print(f"Runtime lib:    {runtime_lib}")
    print(f"Output dir:     {output_dir}")
    print(f"Cases:          {len(sources)}")

    with results_path.open("w", newline="", encoding="utf-8") as results_file, times_path.open(
        "w", newline="", encoding="utf-8"
    ) as times_file, perf_path.open("w", newline="", encoding="utf-8") as perf_file:
        results_writer = csv.writer(results_file, delimiter="\t")
        times_writer = csv.writer(times_file, delimiter="\t")
        perf_writer = csv.writer(perf_file, delimiter="\t")
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
                "opt_level",
                "compile_sec",
                "link_sec",
                "run_sec",
                "suite",
                "source",
            )
        )
        times_writer.writerow(("kind", "run_index", "status", "exit_code", "elapsed_sec", "suite", "source"))
        perf_writer.writerow(("suite", "source", "opt_level", "runs", "mean_sec", "median_sec", "min_sec", "max_sec"))

        for index, src in enumerate(sources, start=1):
            suite = src.parent.name
            opt_level = opt_level_for(suite, args.opt_policy)
            rel = str(src.relative_to(bench_root)).replace("\\", "/")
            asm_file = case_artifact(asm_root, src, bench_root, ".s")
            bin_file = case_artifact(bin_root, src, bench_root, ".exe")
            stdout_file = case_artifact(stdout_root, src, bench_root, ".stdout")
            stderr_file = case_artifact(stdout_root, src, bench_root, ".stderr")
            actual_out_file = case_artifact(stdout_root, src, bench_root, ".actual.out")
            failure_prefix = (failures_root / src.relative_to(bench_root)).with_suffix("")
            for path in (asm_file.parent, bin_file.parent, stdout_file.parent, failure_prefix.parent):
                path.mkdir(parents=True, exist_ok=True)

            print_progress(index, len(sources), src, f"compile {opt_level}")
            compile_cmd = [str(compiler), "-S", "-o", str(asm_file), str(src), opt_level]
            compile_code, compile_stdout, compile_stderr, compile_sec = run_command(compile_cmd, args.timeout)
            compile_status = classify_exit(compile_code)
            write_bytes(stderr_file.with_suffix(".compile.stderr"), compile_stderr)
            if compile_stdout:
                write_bytes(stderr_file.with_suffix(".compile.stdout"), compile_stdout)

            link_status = "SKIP"
            link_code = -1
            link_sec = 0.0
            run_status = "SKIP"
            run_code = -1
            run_sec = 0.0
            compare_status = "SKIP"
            measured_times: list[float] = []

            if compile_status == "OK":
                print_progress(index, len(sources), src, "link")
                link_cmd = [
                    args.linker,
                    "-march=rv64gc",
                    "-mabi=lp64d",
                    "-mcmodel=medany",
                    "-static",
                    str(asm_file),
                    str(runtime_lib),
                    "-o",
                    str(bin_file),
                ]
                link_code, link_stdout, link_stderr, link_sec = run_command(link_cmd, args.timeout)
                link_status = classify_exit(link_code)
                if link_stdout:
                    write_bytes(stderr_file.with_suffix(".link.stdout"), link_stdout)
                if link_stderr:
                    write_bytes(stderr_file.with_suffix(".link.stderr"), link_stderr)
            else:
                write_bytes(failure_prefix.with_suffix(".compile.stderr"), compile_stderr)
                if compile_stdout:
                    write_bytes(failure_prefix.with_suffix(".compile.stdout"), compile_stdout)

            if link_status == "OK":
                stdin_path = src.with_suffix(".in")
                stdin_data = stdin_path.read_bytes() if stdin_path.is_file() else None
                run_cmd = runner_command(args.runner_command, bin_file)
                first_stdout = b""
                first_stderr = b""
                total_runs = args.warmup_runs + args.runs
                for run_index in range(total_runs):
                    kind = "warmup" if run_index < args.warmup_runs else "measure"
                    display_run = run_index + 1 if kind == "warmup" else run_index - args.warmup_runs + 1
                    print_progress(index, len(sources), src, f"run {kind} {display_run}")
                    code, stdout, stderr, elapsed = run_command(run_cmd, args.run_timeout, stdin_data)
                    status = classify_run_exit(code)
                    time_row = {
                        "kind": kind,
                        "run_index": display_run,
                        "status": status,
                        "exit_code": code,
                        "elapsed_sec": f"{elapsed:.6f}",
                        "suite": suite,
                        "source": rel,
                    }
                    time_rows.append(time_row)
                    times_writer.writerow(tuple(time_row.values()))
                    if kind == "measure":
                        measured_times.append(elapsed)
                    if run_index == 0:
                        first_stdout = stdout
                        first_stderr = stderr
                    if status != "OK":
                        run_status = status
                        run_code = code
                        run_sec = elapsed
                        write_bytes(failure_prefix.with_suffix(".run.stderr"), stderr)
                        break
                    run_status = status
                    run_code = code
                    run_sec = elapsed
                    if kind == "measure" and display_run == 1:
                        first_stdout = stdout
                        first_stderr = stderr
                write_bytes(stdout_file, first_stdout)
                write_bytes(stderr_file, first_stderr)

                expected_path = src.with_suffix(".out")
                if run_status == "OK" and args.verify_output:
                    if not expected_path.is_file():
                        compare_status = "NO_EXPECTED"
                    else:
                        compare_status, expected_text, actual_text = compare_output(expected_path, first_stdout, run_code)
                        actual_out_file.write_text(actual_text, encoding="utf-8")
                        if compare_status != "MATCH":
                            failure_prefix.with_suffix(".expected.out").write_text(expected_text, encoding="utf-8")
                            failure_prefix.with_suffix(".actual.out").write_text(actual_text, encoding="utf-8")
                elif run_status == "OK":
                    compare_status = "SKIP"

            status = final_status(compile_status, link_status, run_status, compare_status, args.verify_output)
            result = CaseResult(
                suite=suite,
                source=rel,
                opt_level=opt_level,
                final_status=status,
                compile_status=compile_status,
                compile_exit_code=compile_code,
                link_status=link_status,
                link_exit_code=link_code,
                run_status=run_status,
                run_exit_code=run_code,
                compare_status=compare_status,
                compile_sec=compile_sec,
                link_sec=link_sec,
                run_sec=run_sec,
            )
            results.append(result)
            results_writer.writerow(
                (
                    result.final_status,
                    result.compile_status,
                    result.compile_exit_code,
                    result.link_status,
                    result.link_exit_code,
                    result.run_status,
                    result.run_exit_code,
                    result.compare_status,
                    result.opt_level,
                    f"{result.compile_sec:.6f}",
                    f"{result.link_sec:.6f}",
                    f"{result.run_sec:.6f}",
                    result.suite,
                    result.source,
                )
            )
            if status == "OK" and measured_times:
                perf_row = {
                    "suite": suite,
                    "source": rel,
                    "opt_level": opt_level,
                    "runs": len(measured_times),
                    "mean_sec": f"{statistics.fmean(measured_times):.6f}",
                    "median_sec": f"{statistics.median(measured_times):.6f}",
                    "min_sec": f"{min(measured_times):.6f}",
                    "max_sec": f"{max(measured_times):.6f}",
                }
                perf_rows.append(perf_row)
                perf_writer.writerow(tuple(perf_row.values()))
            print_progress(index, len(sources), src, status, final=True)
            if status != "OK" and not args.keep_going:
                break

    write_summary(output_dir, results, time_rows)
    status_counter = Counter(result.final_status for result in results)
    print(f"Summary: TOTAL={len(results)} " + " ".join(f"{k}={v}" for k, v in sorted(status_counter.items())))
    print(f"Report:  {output_dir / 'report.md'}")
    print(f"Details: {results_path}")
    return 0 if status_counter.get("OK", 0) == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
