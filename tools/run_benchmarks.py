#!/usr/bin/env python3
"""Run the fixed-seed matrix and retain raw observations plus machine metadata."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys

WORKLOADS = ["noncrossing", "immediate", "cancellation_bursts", "shared_level", "large_book", "asymmetric"]
REPORT_FILES = {"metadata.json", "raw.jsonl", "raw.csv", "results.json", "summary.csv", "summary.md"}


def command_output(command: list[str], cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False, timeout=15)
        return (completed.stdout or completed.stderr).strip() if completed.returncode == 0 else "unavailable"
    except (OSError, subprocess.SubprocessError):
        return "unavailable"


def cache_metadata(build: Path) -> dict[str, object]:
    cache = build / "CMakeCache.txt"
    selected = {}
    if cache.exists():
        for line in cache.read_text(errors="replace").splitlines():
            if not line or line[0] in "#/" or "=" not in line or ":" not in line.split("=", 1)[0]:
                continue
            name, value = line.split("=", 1)
            key = name.split(":", 1)[0]
            if not key.endswith("-ADVANCED") and (key in {"CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR"} or key.startswith("CMAKE_CXX_FLAGS") or key.startswith("APEX_")):
                selected[key] = value
    compiler = selected.get("CMAKE_CXX_COMPILER")
    if compiler:
        selected["compiler_version"] = command_output([compiler, "--version"])
    commands = build / "compile_commands.json"
    if commands.exists():
        selected["benchmark_compile_commands"] = [row for row in json.loads(commands.read_text(errors="replace")) if Path(row["file"]).name == "benchmark.cpp"]
    return selected


def verify_build(build_info: dict, allow_unverified: bool) -> str:
    """Reject known Debug/instrumented runs; missing provenance needs an explicit override."""
    build_type = build_info.get("CMAKE_BUILD_TYPE")
    if build_type and build_type != "Release":
        raise ValueError(f"performance matrix requires CMAKE_BUILD_TYPE=Release; found {build_type}")
    if any(str(build_info.get(key, "OFF")).upper() not in {"", "OFF", "FALSE", "NO", "0"}
           for key in ("APEX_SANITIZERS", "APEX_TSAN")):
        raise ValueError("performance matrix requires sanitizers to be disabled")
    command_rows = build_info.get("benchmark_compile_commands", [])
    compile_text = " ".join(str(row.get("command", " ".join(row.get("arguments", [])))) for row in command_rows)
    flags = " ".join(str(build_info.get(key, "")) for key in ("CMAKE_CXX_FLAGS", "CMAKE_CXX_FLAGS_RELEASE"))
    if "-fsanitize" in flags + " " + compile_text:
        raise ValueError("performance matrix requires compile commands without sanitizer instrumentation")
    optimization_flags = re.findall(r"(?:^|\s)(-O(?:[0123sgz]|fast))(?=\s|$)", compile_text)
    if optimization_flags and optimization_flags[-1] == "-O0":
        raise ValueError("performance matrix requires optimization; the effective compile flag is -O0")
    if build_type == "Release" and command_rows and optimization_flags and optimization_flags[-1] not in {"-O0", "-Og"}:
        return "Release cache and optimized benchmark compile command inspected; executable/source correspondence requires a fresh build"
    if not allow_unverified:
        raise ValueError("missing Release/optimized compile-command provenance; use a fresh CMake Release build or explicitly pass --allow-unverified-build")
    return "UNVERIFIED build provenance explicitly allowed by --allow-unverified-build"


def prepare_output(output: Path) -> None:
    existing = sorted(name for name in REPORT_FILES if (output / name).exists())
    if existing:
        raise ValueError("output already contains benchmark evidence; choose a new directory: " + ", ".join(existing))
    output.mkdir(parents=True, exist_ok=True)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def portable_metadata(value: object, root: Path, build: Path, executable: Path, compiler: str | None) -> object:
    """Retain flags and relative layout while removing host-specific absolute prefixes."""
    prefixes = [(str(root), "${REPO}"), (str(build), "${BUILD}"), (str(Path.home()), "${HOME}")]
    if compiler:
        prefixes.append((str(Path(compiler).parent), "${TOOLCHAIN}"))
    prefixes.append((str(executable), "${BENCHMARK}"))
    replacements = []
    for prefix, label in prefixes:
        if prefix in {"", ".", "/", "\\"}:
            continue
        replacements.extend((variant, label) for variant in {prefix, prefix.replace("\\", "/"), prefix.replace("/", "\\")})
    replacements.sort(key=lambda item: len(item[0]), reverse=True)

    def sanitize(text: str) -> str:
        for prefix, label in replacements:
            text = re.sub(re.escape(prefix), lambda _: label, text, flags=re.IGNORECASE if os.name == "nt" else 0)
        # Other absolute path tokens (for example a system include directory)
        # keep their basename. URL paths are excluded by the preceding ':'/'/'.
        pattern = r"(?:(?<![\w:/\\}])|(?<=-I)|(?<=-L)|(?<==))(?:[A-Za-z]:[/\\]|/)[^\s\"'<>|]+"
        text = re.sub(pattern, lambda match: "${EXTERNAL}/" + re.split(r"[/\\]", match.group())[-1], text)
        return text

    def visit(item: object) -> object:
        if isinstance(item, str):
            return sanitize(item)
        if isinstance(item, list):
            return [visit(part) for part in item]
        if isinstance(item, dict):
            return {key: visit(part) for key, part in item.items()}
        return item
    return visit(value)


def metadata(root: Path, build: Path, executable: Path, args: argparse.Namespace, build_info: dict, build_verification: str) -> dict:
    cpu = platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER", "unavailable")
    physical_memory = None
    if os.name == "nt":
        windows_cpu = command_output(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor | Select-Object -ExpandProperty Name) -join '; '"])
        if windows_cpu != "unavailable":
            cpu = windows_cpu
        windows_memory = command_output(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory"])
        if windows_memory.isdecimal():
            physical_memory = int(windows_memory)
    elif hasattr(os, "sysconf"):
        try:
            physical_memory = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
        except (ValueError, OSError):
            pass
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text(errors="replace").splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    source_files = [root / "CMakeLists.txt", root / "CMakePresets.json"]
    for directory in ("src", "include", "benchmarks", "tools"):
        source_files.extend(path for path in (root / directory).rglob("*") if path.is_file() and path.suffix in {".cpp", ".hpp", ".py", ".sh"})
    info = {
        "recorded_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "platform": platform.platform(), "os": platform.system(), "release": platform.release(),
        "machine": platform.machine(), "cpu": cpu, "logical_cpus": os.cpu_count(), "physical_memory_bytes": physical_memory, "cpu_affinity": affinity,
        "python": platform.python_version(), "executable": str(executable), "executable_sha256": sha256_file(executable), "build": build_info,
        "compiler_basename": Path(build_info["CMAKE_CXX_COMPILER"]).name if build_info.get("CMAKE_CXX_COMPILER") else "unavailable",
        "build_verification": build_verification,
        "source_sha256": {path.relative_to(root).as_posix(): sha256_file(path) for path in sorted(source_files)},
        "git_revision": command_output(["git", "rev-parse", "HEAD"], root),
        "git_status": command_output(["git", "status", "--short"], root),
        "invocation": sys.argv, "seed": args.seed, "sizes": args.sizes,
        "workloads": args.workloads, "modes": args.modes, "repeats": args.repeats, "warmups": args.warmups,
        "mode_order": "alternated by workload/size; each mode uses a separate process",
        "timing_scope": "engine add/cancel plus returned event destruction; no parsing, IO, engine construction/destruction, invariant checks, or workload generation",
        "allocation_scope": "separate untimed pass counting book PMR upstream requests with sizes/alignments; excludes ordinary event vectors, pool object, and process RSS",
        "path_policy": "absolute repository/build/toolchain/home prefixes are replaced by placeholders; other absolute path tokens retain only basename",
    }
    return portable_metadata(info, root, build, executable, build_info.get("CMAKE_CXX_COMPILER"))


def validate_observations(rows: list[dict], size: int, workload: str, mode: str, args: argparse.Namespace) -> None:
    if len(rows) != args.repeats or sorted(row.get("repeat", -1) for row in rows) != list(range(args.repeats)):
        raise ValueError("benchmark must emit each requested repeat exactly once")
    counters = ("batch_ns", "timer_pair_median_ns", "allocations", "allocated_bytes_total", "peak_live_requested_bytes",
                "retained_requested_bytes", "emitted_events", "executions", "accepted_adds", "active_orders")
    metrics = ("requests_per_second", "latency_mean_ns", "latency_p50_ns", "latency_p95_ns", "latency_p99_ns")
    for row in rows:
        if row.get("schema_version") != 2:
            raise ValueError("unsupported benchmark schema; rebuild the benchmark with current sources")
        if tuple(row.get(key) for key in ("requests", "workload", "mode", "seed", "warmups")) != (size, workload, mode, args.seed, args.warmups):
            raise ValueError("benchmark output does not match requested configuration")
        if any(type(row.get(key)) is not int or row[key] < 0 for key in counters):
            raise ValueError("benchmark counters must be nonnegative integers")
        if any(not isinstance(row.get(key), (int, float)) or not math.isfinite(row[key]) or row[key] < 0 for key in metrics):
            raise ValueError("benchmark metrics must be finite and nonnegative")
        if row["batch_ns"] == 0 or not math.isclose(row["requests_per_second"], size * 1e9 / row["batch_ns"], rel_tol=1e-9):
            raise ValueError("throughput is inconsistent with requests and batch duration")
        if not row["latency_p50_ns"] <= row["latency_p95_ns"] <= row["latency_p99_ns"]:
            raise ValueError("latency percentiles must be monotonically increasing")
        if not row["retained_requested_bytes"] <= row["peak_live_requested_bytes"] <= row["allocated_bytes_total"]:
            raise ValueError("requested-memory counters are inconsistent")
        if row["active_orders"] > row["accepted_adds"] or row["accepted_adds"] > size:
            raise ValueError("order counts are inconsistent")
        fixed_fields = counters[2:]  # Allocator counts and end-state outcomes are deterministic.
        if any(row[key] != rows[0][key] for key in fixed_fields):
            raise ValueError("repeat allocation counts or outcomes differ for the identical request stream")


def summarize(rows: list[dict]) -> list[dict]:
    groups: dict[tuple, list[dict]] = {}
    for row in rows:
        groups.setdefault((row["workload"], row["requests"], row["mode"]), []).append(row)
    summary = []
    for (workload, requests, mode), values in groups.items():
        summary.append({
            "workload": workload, "requests": requests, "mode": mode, "repeats": len(values),
            "median_requests_per_second": statistics.median(v["requests_per_second"] for v in values),
            "min_requests_per_second": min(v["requests_per_second"] for v in values),
            "max_requests_per_second": max(v["requests_per_second"] for v in values),
            "median_latency_mean_ns": statistics.median(v["latency_mean_ns"] for v in values),
            "median_latency_p50_ns": statistics.median(v["latency_p50_ns"] for v in values),
            "median_latency_p95_ns": statistics.median(v["latency_p95_ns"] for v in values),
            "median_latency_p99_ns": statistics.median(v["latency_p99_ns"] for v in values),
            "allocations": values[0]["allocations"], "peak_live_requested_bytes": values[0]["peak_live_requested_bytes"],
            "retained_requested_bytes": values[0]["retained_requested_bytes"], "active_orders": values[0]["active_orders"],
        })
    return summary


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def markdown_report(summary: list[dict], info: dict) -> str:
    lines = ["# Recorded benchmark observations", "", f"Recorded: {info['recorded_utc']}",
             f"CPU: {info['cpu']}; OS: {info['platform']}", "", 
             "Throughput is the median of repeated batch measurements. Latency columns are medians of per-repeat percentiles, not percentiles of pooled samples.", "",
             "| Workload | Requests | Mode | requests/s | mean ns | p50 ns | p95 ns | p99 ns | allocations | peak requested MiB |",
             "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|"]
    for row in summary:
        lines.append(f"| {row['workload']} | {row['requests']:,} | {row['mode']} | {row['median_requests_per_second']:,.0f} | {row['median_latency_mean_ns']:,.0f} | {row['median_latency_p50_ns']:,.0f} | {row['median_latency_p95_ns']:,.0f} | {row['median_latency_p99_ns']:,.0f} | {row['allocations']:,} | {row['peak_live_requested_bytes'] / 2**20:.2f} |")
    lines += ["", "## Pool / baseline ratios", "", "A throughput ratio above 1 favors the pool; a memory or p99 ratio above 1 means the pool used more memory or had a higher recorded tail.", "",
              "| Workload | Requests | throughput ratio | p99 ratio | allocation-count ratio | peak memory ratio |",
              "|---|---:|---:|---:|---:|---:|"]
    lookup = {(r["workload"], r["requests"], r["mode"]): r for r in summary}
    for row in summary:
        if row["mode"] != "baseline":
            continue
        pool = lookup.get((row["workload"], row["requests"], "pool"))
        if not pool:
            continue
        def ratio(key: str) -> str:
            return f"{pool[key] / row[key]:.3f}" if row[key] else "n/a"
        lines.append(f"| {row['workload']} | {row['requests']:,} | {ratio('median_requests_per_second')} | {ratio('median_latency_p99_ns')} | {ratio('allocations')} | {ratio('peak_live_requested_bytes')} |")
    lines += ["", "These are observations on this machine, not production latency guarantees. Read docs/performance.md for instrumentation and experimental limitations.", ""]
    return "\n".join(lines)


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build" / "release")
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--output", type=Path, default=root / "results" / "benchmark")
    parser.add_argument("--sizes", type=int, nargs="+", default=[10000, 100000, 1000000])
    parser.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=WORKLOADS)
    parser.add_argument("--modes", nargs="+", choices=["baseline", "pool"], default=["baseline", "pool"])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--seed", type=int, default=305419896)
    parser.add_argument("--allow-unverified-build", action="store_true", help="allow missing CMake/compile-command provenance (known Debug/sanitized builds are still rejected)")
    args = parser.parse_args()
    if args.repeats < 1 or args.warmups < 0 or any(not 1 <= n <= 100000000 for n in args.sizes) or not 0 <= args.seed <= 2**64 - 1:
        parser.error("sizes must be 1..100000000, repeats positive, warmups nonnegative, and seed a uint64")
    if any(len(values) != len(set(values)) for values in (args.sizes, args.workloads, args.modes)):
        parser.error("sizes, workloads, and modes must not contain duplicates")
    build = args.build_dir.resolve()
    executable = (args.executable or build / ("apexmatch_bench.exe" if os.name == "nt" else "apexmatch_bench")).resolve()
    if not executable.is_file():
        parser.error(f"benchmark executable does not exist: {executable}")
    output = args.output.resolve()
    try:
        build_info = cache_metadata(build)
        build_verification = verify_build(build_info, args.allow_unverified_build)
        prepare_output(output)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    info = metadata(root, build, executable, args, build_info, build_verification)
    (output / "metadata.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    rows: list[dict] = []
    try:
        with (output / "raw.jsonl").open("w", encoding="utf-8") as raw:
            for index, (size, workload) in enumerate((s, w) for s in args.sizes for w in args.workloads):
                modes = args.modes if index % 2 == 0 else list(reversed(args.modes))
                for mode in modes:
                    command = [str(executable), "--size", str(size), "--workload", workload, "--mode", mode,
                               "--warmups", str(args.warmups), "--repeats", str(args.repeats), "--seed", str(args.seed)]
                    print(f"Measuring {workload}: {size:,} requests, {mode}", flush=True)
                    completed = subprocess.run(command, capture_output=True, text=True, check=True)
                    new_rows = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
                    validate_observations(new_rows, size, workload, mode, args)
                    for row in new_rows:
                        comparable = [other for other in rows if (other["workload"], other["requests"], other["seed"]) == (workload, size, args.seed)]
                        for other in comparable:
                            if any(row[field] != other[field] for field in ("emitted_events", "executions", "accepted_adds", "active_orders")):
                                raise RuntimeError("baseline/pool outcome counts differ")
                    for row in new_rows:
                        raw.write(json.dumps(row, separators=(",", ":")) + "\n")
                    raw.flush()
                    rows.extend(new_rows)
    except (OSError, RuntimeError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        print(f"Benchmark failed; partial raw observations retained at {output}: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        return 1
    summary = summarize(rows)
    write_csv(output / "raw.csv", rows)
    write_csv(output / "summary.csv", summary)
    (output / "results.json").write_text(json.dumps({"metadata": info, "observations": rows, "summary": summary}, indent=2) + "\n", encoding="utf-8")
    (output / "summary.md").write_text(markdown_report(summary, info), encoding="utf-8")
    print(f"Saved raw observations, metadata, CSV, and report to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
