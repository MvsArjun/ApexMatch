#!/usr/bin/env python3
"""Configure, build and test a chosen compiler; save portable logs and pass counts."""
from __future__ import annotations
import argparse
import datetime as dt
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from portable_evidence import redact


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=root)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--config", choices=["Debug", "Release"], default="Release")
    parser.add_argument("--sanitizer", choices=["none", "address", "thread"], default="none")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--parallel", type=int, default=4)
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("--parallel must be positive")
    output = args.output.resolve()
    if output.exists() and any(output.iterdir()):
        parser.error("use a fresh output directory to preserve earlier evidence")
    output.mkdir(parents=True, exist_ok=True)
    source, build = args.source.resolve(), args.build_dir.resolve()
    compiler = subprocess.run([args.compiler, "--version"], capture_output=True, text=True, check=True).stdout
    revision = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], capture_output=True, text=True)
    info = {"schema_version": 1, "started_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "source_revision": revision.stdout.strip() if revision.returncode == 0 else "archive without Git metadata",
            "compiler": redact(compiler, root), "configuration": args.config, "sanitizer": args.sanitizer,
            "commands": [], "result": "incomplete"}
    commands = [
        ("configure", ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                       "-DCMAKE_CXX_COMPILER=" + args.compiler, "-DCMAKE_BUILD_TYPE=" + args.config,
                       "-DAPEX_SANITIZERS=" + ("ON" if args.sanitizer == "address" else "OFF"),
                       "-DAPEX_TSAN=" + ("ON" if args.sanitizer == "thread" else "OFF"),
                       "-DAPEX_BUILD_BENCHMARKS=" + ("ON" if args.sanitizer == "none" else "OFF")]),
        ("build", ["cmake", "--build", str(build), "--parallel", str(args.parallel)]),
        ("test", ["ctest", "--test-dir", str(build), "--output-on-failure", "--output-junit", str(build / "audit-junit.xml")]),
    ]
    status = 0
    for stage, command in commands:
        print(f"{args.compiler}: {args.config} {args.sanitizer} — {stage}", flush=True)
        run = subprocess.run(command, capture_output=True, text=True)
        info["commands"].append({"stage": stage, "arguments": [redact(arg, root) for arg in command], "exit_code": run.returncode})
        (output / (stage + ".log")).write_text(redact(run.stdout + run.stderr, root), encoding="utf-8")
        if run.returncode:
            print(redact(run.stdout + run.stderr, root), file=sys.stderr)
            info["result"] = "failed: " + stage
            status = 1
            break
    if status == 0:
        junit_text = (build / "audit-junit.xml").read_text(encoding="utf-8")
        suite = ET.fromstring(junit_text)
        info["ctest"] = {key: int(suite.attrib.get(key, 0)) for key in ("tests", "failures", "disabled")}
        (output / "junit.xml").write_text(redact(junit_text, root), encoding="utf-8")
        details = (build / "Testing" / "Temporary" / "LastTest.log").read_text(encoding="utf-8")
        (output / "test-details.log").write_text(redact(details, root), encoding="utf-8")
        count = re.search(r"All tests passed \((\d+) named cases\)", details)
        info["named_cpp_cases"] = int(count[1]) if count else None
        info["python_unittest_cases"] = sum(map(int, re.findall(r"Ran (\d+) tests? in", details)))
        info["result"] = "passed"
        print(f"Passed {info['ctest']['tests']} CTest groups; {info['named_cpp_cases']} named C++ and {info['python_unittest_cases']} Python tooling cases", flush=True)
    info["finished_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
    (output / "result.json").write_text(json.dumps(info, indent=2) + "\n", encoding="utf-8")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
