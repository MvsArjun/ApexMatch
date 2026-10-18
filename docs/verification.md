# Engineering-audit verification — 18 October 2026

The application defects found in the audit are fixed, but the complete compiler
matrix is **not green**. Native Clang passes the complete exercised suite.
Native GCC 16.2 builds successfully but crashes in a standard-library pool
allocation-failure regression. The test remains mandatory. Linux, TSan,
LeakSanitizer, CPU sampling and hosted CI remain unverified for the reasons below.

## Recorded environment

Windows 11 x86-64, build 26300; AMD Ryzen 5 6600H (6 cores, 12 logical CPUs),
16,312,721,408 bytes visible memory. Normal desktop session, no affinity or CPU
isolation. CMake 4.4.4, Ninja 1.13.2, Python 3.12.8. Compilers:

* LLVM-MinGW 20261006, Clang 23.1.3, libc++, UCRT.
* w64devkit 2.10.1, GCC 16.2.0, libstdc++, native Windows.

Release flags are `-O3 -DNDEBUG -std=c++20` plus the documented warnings.
Debug uses `-g`. ASan/UBSan adds `-fsanitize=address,undefined`,
`-fno-omit-frame-pointer` and `-fno-sanitize-recover=all`.

## Fresh audit runs

Each directory contains configuration/build/test logs, detailed test output and
a result record. Successful runs include portable JUnit. Failed GCC runs retain
their JUnit and failure logs as well.

| Configuration | Exact CTest result | Evidence |
|---|---:|---|
| Clang Debug | 6 passed, 0 failed | [result](../results/audit-verification/clang-debug/result.json), [details](../results/audit-verification/clang-debug/test-details.log) |
| Clang Release | 6 passed, 0 failed | [result](../results/audit-verification/clang-release/result.json), [details](../results/audit-verification/clang-release/test-details.log) |
| Clang Debug ASan + UBSan | 6 passed, 0 failed | [result](../results/audit-verification/clang-asan/result.json), [details](../results/audit-verification/clang-asan/test-details.log) |
| Final clean Git clone, Clang Release | 6 passed, 0 failed | [result](../results/audit-verification/clean-clone/result.json), [details](../results/audit-verification/clean-clone/test-details.log) |
| GCC Debug | 5 passed, 1 failed (`core_and_extensions`, SIGSEGV) | [result](../results/audit-verification/gcc-debug/result.json), [test output](../results/audit-verification/gcc-debug/test.log) |
| GCC Release | 5 passed, 1 failed (`core_and_extensions`, SIGSEGV) | [result](../results/audit-verification/gcc-release/result.json), [test output](../results/audit-verification/gcc-release/test.log) |

Every complete Clang run passed **29 named C++ cases**, **13 Python tooling
cases** (8 benchmark, 5 release-scan), and the replay, TCP and executable-example
integration groups. The C++ suite includes **40,000 differential requests**:
four fixed seeds, 5,000 requests each, baseline and pooled allocation, comparing
the complete book, sequences, events, exact rejection reasons and statistics.
An independent per-ID ledger checks quantity conservation and overexecution.

The six CTest groups are `core_and_extensions`, `replay`, `network`,
`extension_examples`, `benchmark_tooling`, and `release_tooling`. CTest groups,
named cases and randomized requests are different counts and must not be added
together as if they were the same unit.

The GCC group terminates during allocation-failure injection before completing
all named cases. Its five passing integration/tooling groups do not establish
that the full GCC correctness suite passed. No assertion or test was disabled.

The initial repository at `bf0e409` was also archived into an independent source
directory, freshly configured/built, and passed its original four CTest groups
before audit changes. Historical delivery evidence (26 named cases) remains in
[results/verification](../results/verification). Current coverage is broader.
The final clean clone at `7b47fb7` passed all six groups, 29 named C++ cases and
13 tooling cases using only committed source; subsequent edits are documentation
and evidence updates. The clone's verification record includes source hashes,
clean Git status and the actual compiler/version/commands.

The audited Release benchmark completed 108 observations at clean source
revision `40ead65`. At one-million noncrossing requests, median throughput was
1.833M requests/s baseline and 3.074M pooled, p99 800/700 ns, and peak requested
book memory 147.34/199.41 MiB. The full [current matrix](../results/audit-native-clang23/summary.md)
and [methodology](performance.md) preserve repeat ranges and limits alongside
the original historical run. These are normal-allocation Clang measurements,
not evidence that the failed GCC resource-exhaustion test passed.

## Reproduced GCC dependency failure

The new test allows two upstream PMR allocations, then throws `std::bad_alloc`.
In the tested GCC 16.2 libstdc++ pool implementation, a failed chunk-metadata
allocation is caught without rethrowing; allocation then accesses an empty
chunk vector. GDB identifies `unsynchronized_pool_resource::do_allocate`.
A [standalone reproducer](../tools/repro_pool_failure.cpp) uses **no ApexMatch
engine code** and reproduces the crash. The same probe propagates `bad_alloc`
under the tested Clang/libc++ runtime. See [the issue analysis](toolchain-issue.md)
for exact commands, output, source references and the limits of that diagnosis.

This is a release blocker for the tested GCC/runtime combination under resource
exhaustion. It does not justify replacing the working matching architecture,
pretending GCC passed, or excluding the regression. Other GCC versions and
platforms have not been tested for this defect here. No upstream issue was sent.

## Sanitizers and unavailable checks

The complete native Clang ASan/UBSan run passed with no diagnostic. Environment:
`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and
`UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. The C++ group took 80.20 s;
the six groups took 111.33 s. Injected allocation failures cover book-resource
allocations and safe destruction on the tested libc++; they do not prove
transactional rollback or cover every global event-vector allocation.

| Check | Precise blocker / result |
|---|---|
| Linux GCC/Clang | WSL reports not installed; no Linux kernel/container runtime is available. Native Windows GCC is a separate result. |
| TSan | Not supported by the native target; Linux `tsan` preset and CI job supplied, not executed here. |
| LeakSanitizer | Native runtime reports `detect_leaks is not supported on this platform`. Linux `asan` preset enables it; no leak-detection pass is claimed. |
| Linux `perf` | No Linux kernel or perf. [Profiling reproduction](performance.md#profiling) supplied. |
| Windows CPU sampling | WPR was not recording. `wpr -start CPU -filemode` failed with performance-policy error `0xc5585011`. It remained not recording; no trace was collected. [Record](../results/audit-verification/profiling-status.json). |
| Workflow syntax | actionlint 1.7.12 passed with ShellCheck disabled because that optional tool is absent. This validates workflow structure, not job success. |
| Hosted CI | No remote/push/workflow run exists. Six compiler/configuration jobs and separate TSan job are configured; no green CI claim is made. |

Allocation census and timings are actual local measurements. Sampled CPU stacks,
cache/branch counters and measured CPU hotspot percentages are unavailable.

## Reproduce without machine-specific paths

Install the README prerequisites and put a modern compiler, CMake, Ninja and
Python on PATH. Use a fresh build/output directory for each compiler and run.
For a portable evidence record:

```sh
python3 tools/verify_build.py --compiler clang++ --config Release \
  --build-dir build/local-clang --output results/local/clang-release
python3 tools/verify_build.py --compiler g++ --config Debug \
  --build-dir build/local-gcc --output results/local/gcc-debug
```

Use `python` in PowerShell. For the Windows sanitizer run:

```powershell
$env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
$env:UBSAN_OPTIONS = 'print_stacktrace=1:halt_on_error=1'
python tools/verify_build.py --compiler clang++ --config Debug --sanitizer address --build-dir build/local-asan --output results/local/clang-asan
```

The simpler presets remain available:

```sh
cmake --preset release -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset release --parallel
ctest --preset release
cmake --preset asan -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset asan --parallel
ctest --preset asan          # Linux, includes leak detection
```

Native Windows uses `ctest --preset asan-windows` after the same ASan build.
The verification helper makes empty test discovery fail, refuses overwriting
earlier evidence, and records source hashes in new runs. Sanitized test timeouts
are 300 seconds to allow instrumentation overhead without weakening assertions.

## Historical evidence, licensing and publication

The original 108 observations remain in [results/native-clang23](../results/native-clang23).
Only personal absolute prefixes in historical JSON metadata/logs were redacted;
raw CSV/JSONL and numeric observations were preserved. The unchanged numerical
digest is recorded in [historical-preservation.json](../results/audit-verification/historical-preservation.json).
Git history was not rewritten and still contains original personal path strings.

MIT was explicitly confirmed during this audit; the existing license was kept.
There is no Git remote and no GitHub CLI executable in PATH. The connected
GitHub account identifies as `MvsArjun`, which does not establish local Git push
authentication or permission to publish. No repository was created or pushed.
See [release preparation](release-preparation.md) for review items and approval
requirements before any publication.
