#!/usr/bin/env python3
"""Run the fixed-seed matrix and retain raw observations plus machine metadata."""
from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

WORKLOADS = ["noncrossing", "immediate", "cancellation_bursts", "shared_level", "large_book", "asymmetric"]


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
            if key in {"CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE", "CMAKE_GENERATOR"} or key.startswith("CMAKE_CXX_FLAGS") or key.startswith("APEX_"):
                selected[key] = value
    compiler = selected.get("CMAKE_CXX_COMPILER")
    if compiler:
        selected["compiler_version"] = command_output([compiler, "--version"])
    commands = build / "compile_commands.json"
    if commands.exists():
        selected["benchmark_compile_commands"] = [row for row in json.loads(commands.read_text(errors="replace")) if Path(row["file"]).name == "benchmark.cpp"]
    return selected


def metadata(root: Path, build: Path, executable: Path, args: argparse.Namespace) -> dict:
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
    return {
        "recorded_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "platform": platform.platform(), "os": platform.system(), "release": platform.release(),
        "machine": platform.machine(), "cpu": cpu, "logical_cpus": os.cpu_count(), "physical_memory_bytes": physical_memory, "cpu_affinity": affinity,
        "python": platform.python_version(), "executable": str(executable), "build": cache_metadata(build),
        "git_revision": command_output(["git", "rev-parse", "HEAD"], root),
        "git_status": command_output(["git", "status", "--short"], root),
        "invocation": sys.argv, "seed": args.seed, "sizes": args.sizes,
        "workloads": args.workloads, "modes": args.modes, "repeats": args.repeats, "warmups": args.warmups,
        "mode_order": "alternated by workload/size; each mode uses a separate process",
        "timing_scope": "engine add/cancel plus returned event destruction; no parsing, IO, engine construction/destruction, invariant checks, or workload generation",
        "allocation_scope": "separate untimed pass counting book PMR upstream requests with sizes/alignments; excludes ordinary event vectors, pool object, and process RSS",
    }


def summarize(rows: list[dict]) -> list[dict]:
    groups: dict[tuple, list[dict]] = {}
    for row in rows:
        groups.setdefault((row["workload"], row["requests"], row["mode"]), []).append(row)
    summary = []
    for (workload, requests, mode), values in groups.items():
        summary.append({
            "workload": workload, "requests": requests, "mode": mode, "repeats": len(values),
            "median_events_per_second": statistics.median(v["events_per_second"] for v in values),
            "min_events_per_second": min(v["events_per_second"] for v in values),
            "max_events_per_second": max(v["events_per_second"] for v in values),
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
        lines.append(f"| {row['workload']} | {row['requests']:,} | {row['mode']} | {row['median_events_per_second']:,.0f} | {row['median_latency_mean_ns']:,.0f} | {row['median_latency_p50_ns']:,.0f} | {row['median_latency_p95_ns']:,.0f} | {row['median_latency_p99_ns']:,.0f} | {row['allocations']:,} | {row['peak_live_requested_bytes'] / 2**20:.2f} |")
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
        lines.append(f"| {row['workload']} | {row['requests']:,} | {ratio('median_events_per_second')} | {ratio('median_latency_p99_ns')} | {ratio('allocations')} | {ratio('peak_live_requested_bytes')} |")
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
    args = parser.parse_args()
    if args.repeats < 1 or args.warmups < 0 or any(n < 1 for n in args.sizes):
        parser.error("sizes/repeats must be positive and warmups nonnegative")
    build = args.build_dir.resolve()
    executable = (args.executable or build / ("apexmatch_bench.exe" if os.name == "nt" else "apexmatch_bench")).resolve()
    if not executable.is_file():
        parser.error(f"benchmark executable does not exist: {executable}")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    info = metadata(root, build, executable, args)
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
                    if len(new_rows) != args.repeats:
                        raise RuntimeError(f"expected {args.repeats} observations; received {len(new_rows)}")
                    for row in new_rows:
                        if (row["requests"], row["workload"], row["mode"], row["seed"]) != (size, workload, mode, args.seed):
                            raise RuntimeError("benchmark output does not match requested configuration")
                        comparable = [other for other in rows if (other["workload"], other["requests"], other["seed"]) == (workload, size, args.seed)]
                        for other in comparable:
                            if any(row[field] != other[field] for field in ("emitted_events", "executions", "accepted_adds", "active_orders")):
                                raise RuntimeError("baseline/pool outcome counts differ")
                    for row in new_rows:
                        raw.write(json.dumps(row, separators=(",", ":")) + "\n")
                    raw.flush()
                    rows.extend(new_rows)
    except (OSError, RuntimeError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
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
