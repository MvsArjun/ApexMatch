#!/usr/bin/env python3
"""Regression checks for evidence preservation and benchmark data integrity."""
from __future__ import annotations

import argparse
import copy
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("run_benchmarks", Path(__file__).resolve().parents[1] / "tools" / "run_benchmarks.py")
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class BenchmarkToolingTests(unittest.TestCase):
    def setUp(self):
        self.args = argparse.Namespace(repeats=3, seed=5, warmups=1)
        self.rows = [{
            "schema_version": 2, "workload": "noncrossing", "requests": 10, "mode": "baseline", "seed": 5,
            "repeat": repeat, "warmups": 1, "batch_ns": elapsed, "requests_per_second": 10 * 1e9 / elapsed,
            "latency_mean_ns": 100.0, "latency_p50_ns": 80, "latency_p95_ns": 120, "latency_p99_ns": p99,
            "timer_pair_median_ns": 0, "allocations": 20, "allocated_bytes_total": 200,
            "peak_live_requested_bytes": 160, "retained_requested_bytes": 120,
            "emitted_events": 20, "executions": 0, "accepted_adds": 10, "active_orders": 10,
        } for repeat, (elapsed, p99) in enumerate(((1000, 150), (2000, 200), (100000, 10000)))]

    def validate(self, rows):
        TOOL.validate_observations(rows, 10, "noncrossing", "baseline", self.args)

    def test_release_provenance_and_missing_provenance(self):
        release = {"CMAKE_BUILD_TYPE": "Release", "benchmark_compile_commands": [{"command": "clang++ -O3 -DNDEBUG -c benchmark.cpp"}]}
        self.assertIn("Release", TOOL.verify_build(release, False))
        with self.assertRaisesRegex(ValueError, "provenance"):
            TOOL.verify_build({}, False)
        self.assertIn("UNVERIFIED", TOOL.verify_build({}, True))

    def test_known_debug_and_instrumented_builds_cannot_be_overridden(self):
        for info in ({"CMAKE_BUILD_TYPE": "Debug"}, {"APEX_SANITIZERS": "ON"}, {"APEX_TSAN": "ON"},
                     {"CMAKE_CXX_FLAGS": "-fsanitize=address"},
                     {"CMAKE_BUILD_TYPE": "Release", "benchmark_compile_commands": [{"command": "clang++ -O3 -DNDEBUG -O0 -c benchmark.cpp"}]},
                     {"benchmark_compile_commands": [{"command": "clang++ -O3 -fsanitize=undefined"}]}):
            with self.subTest(info=info), self.assertRaises(ValueError):
                TOOL.verify_build(info, True)

    def test_existing_evidence_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            summary = output / "summary.md"
            summary.write_text("historical evidence", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "new directory"):
                TOOL.prepare_output(output)
            self.assertEqual(summary.read_text(encoding="utf-8"), "historical evidence")
            self.assertFalse((output / "raw.jsonl").exists())

    def test_portable_metadata_preserves_flags_urls_and_layout(self):
        with tempfile.TemporaryDirectory(prefix="benchmark private ") as directory:
            root = Path(directory).resolve()
            build = root / "build" / "release"
            executable = build / "apexmatch_bench"
            compiler = root.parent / "toolchain" / "clang++"
            original = {"command": f'"{compiler}" -I"{root / "include"}" -I/var/external/include -O3 -DNDEBUG -c "{root / "benchmarks" / "benchmark.cpp"}"',
                        "url": "https://github.com/llvm/llvm-project.git", "output": str(executable)}
            portable = TOOL.portable_metadata(original, root, build, executable, str(compiler))
            self.assertNotIn(str(root), portable["command"])
            self.assertNotIn(str(compiler.parent), portable["command"])
            self.assertNotIn("/var/external", portable["command"])
            self.assertIn("-O3 -DNDEBUG", portable["command"])
            self.assertIn("benchmark.cpp", portable["command"])
            self.assertEqual(portable["url"], original["url"])
            self.assertEqual(portable["output"], "${BENCHMARK}")

    def test_valid_observations_and_summary_medians(self):
        self.validate(self.rows)
        summary = TOOL.summarize(self.rows)[0]
        self.assertEqual(summary["median_requests_per_second"], 5_000_000)
        self.assertEqual(summary["min_requests_per_second"], 100_000)
        self.assertEqual(summary["median_latency_p99_ns"], 200)

    def test_duplicate_and_missing_repeats_are_rejected(self):
        for repeats in ((0, 0, 2), (0, 1)):
            rows = copy.deepcopy(self.rows[:len(repeats)])
            for row, repeat in zip(rows, repeats):
                row["repeat"] = repeat
            with self.subTest(repeats=repeats), self.assertRaises(ValueError):
                self.validate(rows)

    def test_corrupt_numeric_observations_are_rejected(self):
        changes = (("requests_per_second", float("inf")), ("latency_mean_ns", float("nan")),
                   ("batch_ns", 0), ("requests_per_second", 1234), ("allocations", -1),
                   ("latency_p95_ns", 1), ("retained_requested_bytes", 999), ("active_orders", 11))
        for key, value in changes:
            rows = copy.deepcopy(self.rows)
            rows[0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.validate(rows)

    def test_wrong_configuration_and_outcome_drift_are_rejected(self):
        for key, value in (("schema_version", 1), ("warmups", 2), ("seed", 6), ("mode", "pool"),
                           ("emitted_events", 21), ("allocations", 21)):
            rows = copy.deepcopy(self.rows)
            rows[1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(rows)


if __name__ == "__main__":
    unittest.main()
