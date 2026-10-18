# Delivery verification — 2026-10-18

All requested components are implemented. The results below establish native
Windows verification; they do not claim Linux/WSL2, remote CI, TSan, leak
detection, or Linux CPU profiling ran on this host.

## Environment

* Windows 11 Home Single Language, version 10.0.26300, x86-64.
* AMD Ryzen 5 6600H, 6 physical cores / 12 logical processors; 16,312,721,408
  bytes visible RAM. Normal desktop session, no CPU affinity or isolation.
* LLVM-MinGW 20261006: Clang 23.1.3, libc++, UCRT, native x86_64 Windows target.
* CMake 4.4.4, Ninja 1.13.2, Python 3.12.8.
* Release compiler options: `-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Wshadow`.
* Debug/sanitizer builds use `-g`; sanitizer flags add
  `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`.

The original PATH compiler was GCC 6.3, which cannot build this project as C++20.
A separate LLVM-MinGW toolchain was installed; the existing compiler and synced
project reference files were left in place. WSL reports that it is not installed.

## Checks actually run

| Check | Result / evidence |
|---|---|
| Native debug build and original three CTest groups | Passed; 26 named C++ cases, replay and networking. |
| Native release, final four CTest groups | All passed: 26 named C++ cases, replay, networking, pipeline/TWAP executable examples. [Log](../results/verification/release.log). |
| Independent flat-vector reference oracle | 40,000 requests: four fixed seeds, 5,000 requests each, baseline and pool. Full events, book, statistics, invariants and per-order conservation compared. Included in the C++ test group. |
| ASan + UBSan, core/extensions, replay and TCP | All three groups passed, no sanitizer finding. Core group took 75.16 seconds. [Log](../results/verification/asan-core-replay-network.log). |
| ASan + UBSan, pipeline/TWAP executable examples | Passed after adding the fourth CTest group. [Log](../results/verification/asan-extension-examples.log). The other three groups were already tested with the same library sources. |
| ASan + UBSan, expanded large-response TCP check | Passed after adding a 600-maker sweep through the delivered client, exercising multiple server writes and client reads. [Log](../results/verification/asan-network-large-response.log). |
| Clean Git clone release configure/build/test | Passed all four groups in a new local clone, without using untracked source files or prior build products. [Log](../results/verification/clean-checkout-release.log). |
| Deterministic replay | Repeated file/stdin replay, golden output and pool/default equivalence all passed. |
| Bounded pipeline | Sequential equivalence, finite capacity, FIFO, 50,000 ordered queue items, two producers, shutdown/draining, error propagation and oversized-line recovery passed. |
| Market data | Delivered JSON incrementals reconstructed against snapshots and BOOK; partial fills, multiple levels, removals and top quotes passed. |
| Local TCP | Fragmented/coalesced input, malformed messages, line limits, half-close, abrupt close, bundled client, output bound and explicit draining shutdown passed. Native forced termination does not test POSIX SIGTERM. |
| TWAP | Schedule/rounding, ID/time boundaries, rejection collisions, later maker fills, terminal/external cancellation and repeatable executable fixtures passed. Fixture: 23 submitted, 13 executed, 10 cancelled. |
| Benchmark matrix | Six workloads × three sizes × two modes × three repeats = 108 recorded observations, each with separate batch and latency passes plus allocation census. All completed; outcomes agreed across passes and instrumented memory returned to zero after destruction. [Data](../results/native-clang23/results.json). |

Initial clean-clone verification used source commit `2100021`; it includes the
final engine, extensions and benchmark implementation. The clone was then
advanced to the delivery commit and retested with the expanded large-response
network check. Later changes add documentation, recorded results, tests and a
separate profile configuration; they do not change matching behavior or the
benchmark's release compilation.
Benchmark metadata records its exact revision and clean Git status at run start.

## Checks unavailable here

| Check | Specific limitation | Reproduction |
|---|---|---|
| Linux/WSL2 GCC and Clang builds/tests | WSL/Linux kernel not installed on this Windows host. | Linux dependency/build commands in README; CI matrix in `.github/workflows/ci.yml`. |
| ThreadSanitizer | Native Windows target does not support this configuration; no Linux kernel available. | `cmake --preset tsan -DCMAKE_CXX_COMPILER=clang++`, build and `ctest --preset tsan` in Linux. |
| LeakSanitizer | This Windows ASan runtime reports `detect_leaks is not supported on this platform`. | Linux `asan` preset enables `ASAN_OPTIONS=detect_leaks=1`. |
| Linux `perf` CPU counters/stacks | No Linux kernel or `perf` executable. | Build `profile` preset, then `bash tools/profile.sh build/profile noncrossing 1000000 results/profile`. Kernel-compatible perf and permission required. |
| GitHub-hosted CI | Repository has no configured remote or workflow run. | Push to a GitHub repository to run the supplied workflow. |

The measured allocation census is an instrumentation profile of upstream book
allocation demand. It is not a sampled CPU profile. No claim is made about cache
misses, branch misses, cycles per instruction, or which function dominated CPU
time without those unavailable counters/stacks.

## Exact native commands used

From the repository root in PowerShell on this machine:

```powershell
$env:PATH = 'C:\Users\mvsar\.codex\tools\llvm-mingw-20261006-ucrt-x86_64\bin;C:\Users\mvsar\AppData\Roaming\Python\Python312\Scripts;' + $env:PATH
cmake --preset debug -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset debug --parallel 6
ctest --preset debug
cmake --preset release -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset release --parallel 6
ctest --preset release
cmake --preset asan -DCMAKE_CXX_COMPILER=clang++
cmake --build --preset asan --parallel 6
$env:ASAN_OPTIONS = 'detect_leaks=0:halt_on_error=1'
$env:UBSAN_OPTIONS = 'print_stacktrace=1:halt_on_error=1'
ctest --test-dir build/asan --output-on-failure
python tools/run_benchmarks.py --build-dir build/release --output results/my-rerun --repeats 3
```

The Linux-oriented `ctest --preset asan` sets leak detection on. Native Windows
uses `ctest --test-dir` so that it retains the explicitly disabled unsupported
leak check. Address and undefined-behavior instrumentation are still active.
The final sanitizer evidence is split across two logs because the executable
example group was added after the first three groups passed.

For the clean clone:

```powershell
git clone --no-local . build/clean-checkout
cmake -S build/clean-checkout -B build/clean-checkout/build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build/clean-checkout/build/release --parallel 6
ctest --test-dir build/clean-checkout/build/release --output-on-failure
```

Use a new clone directory if repeating that check. Build outputs and the
verification clone are ignored by Git; only source, examples, documentation and
recorded evidence are committed.

## Issues resolved during verification

The networking integration test originally attempted to reconnect after it had
already requested shutdown. It now waits for the draining server to exit.
The backpressure check uses a deterministic single-response bound rather than
assuming a particular TCP receive segmentation.

The first allocation probe globally replaced C++ `new`/`delete` and encountered
heap corruption when combined with the native libc++ runtime. That probe was
removed before recorded measurements. The delivered probe injects a counting
PMR upstream only into a separate untimed pass, forwarding size/alignment and
checking complete release at destruction. The full benchmark matrix completed
with that implementation. No failed smoke-run data are presented as results.

The remaining limitations are environment-specific verification and the
intentional scope documented in README; no required component is a placeholder.
