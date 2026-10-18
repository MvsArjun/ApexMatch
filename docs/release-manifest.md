# Release manifest

Compared with pre-audit revision f0e409. All listed changes are local audit commits; no remote or publication exists. Existing history is preserved.

The final handoff has no pending staged or unstaged changes. This manifest includes source, tests, documentation and small reproducibility evidence; build trees, binaries, compiler packages and profiling traces are excluded.

| Status | File |
|---|---|
| M | .github/workflows/ci.yml |
| M | .gitignore |
| M | CMakeLists.txt |
| M | CMakePresets.json |
| M | README.md |
| M | benchmarks/benchmark.cpp |
| A | docs/audit-report.md |
| A | docs/benchmark-audit.md |
| A | docs/core-audit.md |
| M | docs/design.md |
| A | docs/extensions-audit.md |
| M | docs/extensions.md |
| A | docs/interview-practice.md |
| M | docs/interview.md |
| M | docs/performance.md |
| A | docs/project-walkthrough.md |
| A | docs/release-manifest.md |
| A | docs/release-preparation.md |
| A | docs/resume-notes.md |
| M | docs/study-guide.md |
| M | docs/testing.md |
| A | docs/toolchain-issue.md |
| M | docs/verification.md |
| M | include/apexmatch/engine.hpp |
| M | include/apexmatch/protocol.hpp |
| A | results/audit-native-clang23/metadata.json |
| A | results/audit-native-clang23/raw.csv |
| A | results/audit-native-clang23/raw.jsonl |
| A | results/audit-native-clang23/results.json |
| A | results/audit-native-clang23/summary.csv |
| A | results/audit-native-clang23/summary.md |
| A | results/audit-verification/clang-asan/build.log |
| A | results/audit-verification/clang-asan/configure.log |
| A | results/audit-verification/clang-asan/junit.xml |
| A | results/audit-verification/clang-asan/result.json |
| A | results/audit-verification/clang-asan/test-details.log |
| A | results/audit-verification/clang-asan/test.log |
| A | results/audit-verification/clang-debug/build.log |
| A | results/audit-verification/clang-debug/configure.log |
| A | results/audit-verification/clang-debug/junit.xml |
| A | results/audit-verification/clang-debug/result.json |
| A | results/audit-verification/clang-debug/test-details.log |
| A | results/audit-verification/clang-debug/test.log |
| A | results/audit-verification/clang-release/build.log |
| A | results/audit-verification/clang-release/configure.log |
| A | results/audit-verification/clang-release/junit.xml |
| A | results/audit-verification/clang-release/result.json |
| A | results/audit-verification/clang-release/test-details.log |
| A | results/audit-verification/clang-release/test.log |
| A | results/audit-verification/gcc-debug/build.log |
| A | results/audit-verification/gcc-debug/configure.log |
| A | results/audit-verification/gcc-debug/junit.xml |
| A | results/audit-verification/gcc-debug/result.json |
| A | results/audit-verification/gcc-debug/test-details.log |
| A | results/audit-verification/gcc-debug/test.log |
| A | results/audit-verification/gcc-release/build.log |
| A | results/audit-verification/gcc-release/configure.log |
| A | results/audit-verification/gcc-release/junit.xml |
| A | results/audit-verification/gcc-release/result.json |
| A | results/audit-verification/gcc-release/test-details.log |
| A | results/audit-verification/gcc-release/test.log |
| A | results/audit-verification/historical-preservation.json |
| A | results/audit-verification/pool-probe/clang.log |
| A | results/audit-verification/pool-probe/gcc-gdb.log |
| A | results/audit-verification/pool-probe/gcc.log |
| A | results/audit-verification/pool-probe/result.json |
| A | results/audit-verification/profiling-status.json |
| A | results/audit-verification/release-scan.json |
| A | results/audit-verification/tooling-checks.json |
| A | results/native-clang23/README.md |
| M | results/native-clang23/metadata.json |
| M | results/native-clang23/results.json |
| M | results/verification/asan-core-replay-network.log |
| M | results/verification/asan-extension-examples.log |
| M | results/verification/asan-network-large-response.log |
| M | results/verification/clean-checkout-release.log |
| M | results/verification/release.log |
| M | src/engine.cpp |
| M | src/extensions.cpp |
| M | src/protocol.cpp |
| A | src/sequence_capacity.hpp |
| A | tests/benchmark_tooling_test.py |
| M | tests/core_tests.cpp |
| M | tests/extensions_tests.cpp |
| M | tests/network_test.py |
| M | tests/reference_model.hpp |
| A | tests/release_tooling_test.py |
| M | tests/test_main.cpp |
| A | tools/portable_evidence.py |
| A | tools/release_audit.py |
| A | tools/repro_pool_failure.cpp |
| M | tools/run_benchmarks.py |
| A | tools/verify_build.py |

Review with git status --short, git log --oneline bf0e409..HEAD, and git diff --stat bf0e409..HEAD.
The exact final commit is available from git rev-parse HEAD; the benchmark records its own measured revision.
