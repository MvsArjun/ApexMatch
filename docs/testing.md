# Correctness and verification

`tests/core_tests.cpp` uses the small throwing `REQUIRE` harness in
`tests/test_support.hpp`. Every named test runs in the normal CTest suite, and a
failed expression reports its source location. Tests are enabled in release
builds as well: they do not rely on `assert`, which `NDEBUG` can remove.

The deterministic cases cover both incoming sides, maker-price execution,
non-crossing books, better-price priority, FIFO priority independent of order ID,
partially filled makers retaining arrival sequence, multiple fills and price
levels, incoming residuals, aggregate depth, final-level deletion, cancellation
of middle FIFO nodes, cancellation before and after fills, inactive cancellation,
duplicate and permanently retired IDs, rejected-ID reuse, invalid side values,
zero/negative values, integer boundaries, exact event order, query sequencing,
strict parsing, JSON escaping, and replay equivalence across allocators.

## Independent randomized oracle

`tests/reference_model.hpp` deliberately stores every active order in one flat,
unsorted vector. It selects each maker by scanning all orders for the best
compatible price and then earliest arrival; cancellation is a linear scan.
Depth is rebuilt from that vector. It shares public value types with the engine,
but does not call its validator or reuse its map/list/index matching algorithm.
This independence makes differential comparisons useful for catching structural
and FIFO-removal bugs rather than comparing two copies of the same algorithm.

The seeds are `0`, `42`, `20261018`, and `0xA9E5C0FFEE`. Each generates 5,000
requests for both `Baseline` and `Pool`: **40,000 differential requests** in all.
The test uses raw `mt19937_64` values and integer remainder rather than a standard
distribution, so the request sequence is stable across standard-library
distribution implementations. Requests include active and inactive
cancellations, duplicates, invalid values, changing prices, and partial fills.

After every request the suite compares acceptance, event kinds and sequence
numbers, complete executions, affected-level quantities/order counts, complete
active-order contents including arrival, full and limited depth, top of book,
and every statistics counter. Rejection reasons are compared exactly against
the independent model. The production
engine's invariant checks run after each request. Failures print seed, request
index, and allocation mode for direct reproduction.

A separate per-ID accounting ledger, updated from the engine's emitted events,
checks that each order satisfies

```
submitted = executed + cancelled + currently resting
```

and never executes beyond its original quantity. Globally, each execution uses
quantity from both counterparties, so the identity is

```
submitted_quantity = 2 * traded_quantity + cancelled_quantity + resting_quantity
```

The maximum accepted lifetime quantity is `UINT64_MAX`; boundary tests exercise
large same-level sums and large executions without signed arithmetic overflow.

## Engineering-audit additions and actual status

The current suite defines 25 core named cases plus four extension groups:
29 named C++ cases in a complete run. Three added core cases cover unknown
typed enum/null-resource arguments, the actual internal sequence preflight at
maximum boundaries, and allocation-failure destruction at 64 successive
upstream allowances per allocation mode. No production counter setter was added.
The oracle now checks exact rejection reasons rather than just nonempty text.

Queue coverage now includes two producers and two consumers; pipeline input
errors drain and propagate. Real-socket client regressions accept the exact
1 MiB response boundary and reject the next byte even with newline framing.
The client rejects command byte 4097 before waiting for newline or EOF. TWAP
adds zero-duration stable ties and maximum quantity/time/ID boundary cases.

CTest also runs eight benchmark-tooling and five release-tooling unit cases.
These validate build provenance, evidence preservation, observation arithmetic,
portable metadata, credential/path/artifact reporting and read-only history scans.
The six CTest groups include the replay, TCP and pipeline/TWAP program checks.

Native Clang Debug, Release and combined ASan/UBSan each passed all six groups,
29 named C++ cases and 13 Python tooling cases. Native GCC 16.2 built but failed
one of six groups in the new pool allocation-failure case. An engine-free
[standard-library reproducer](toolchain-issue.md) exhibits the same crash. The
regression remains mandatory; five other passing GCC groups do not constitute
a complete GCC pass. See [verification.md](verification.md) for logs and counts.

Hosted Linux [run 39149291801](https://github.com/MvsArjun/ApexMatch/actions/runs/39149291801)
at commit `8337a3d91200e990ab0586859871606ffe8136e4` completed four successful
Clang 21.1.8/libc++ 21.1.8 jobs: Debug, Release, ASan/UBSan and separate TSan.
Each passed all six CTest groups with no skipped tests, including all 29 named
C++ cases, 40,000 differential requests and 13 Python tooling cases. The ASan
job used `detect_leaks=1`; no sanitizer diagnostic was observed in the passing
ASan/UBSan or TSan logs. This is actual hosted Linux execution, distinct from
the native Windows results.

Hosted passing logs print CTest group totals; the named-case, randomized-request
and Python-case counts above come from the unchanged test inventory at the
tested commit, rather than separate lines in the passing hosted log.

The original seven Ubuntu 24.04 jobs remain failed: GCC 13.3.0 Debug/Release/ASan
and Clang 18.1.3 Debug/Release/ASan/TSan, using runner-default libstdc++. Each
passed five groups but failed the mandatory allocation-failure C++ case; each
engine-free library probe also segfaulted. The exact shared runtime package
version was not recorded. Four independent libc++ passes therefore do not make
the overall workflow green. See [toolchain-issue.md](toolchain-issue.md) and
[recorded hosted evidence](../results/hosted-ci/39149291801/README.md).

## End-to-end replay

`tests/replay_test.py` launches the actual simulator, verifies that every output
line parses as a JSON object, compares two independent replays byte for byte,
checks `examples/sample.expected.jsonl`, and compares file input with stdin.
The pool allocation mode with request-by-request invariant checking must also
produce exactly the same golden output.
It checks a final command without a newline and a rejected oversized record
followed by a valid one. The golden file is committed, so an accidental protocol
change requires an explicit reviewed update.

## Reproduce

From the repository root in Linux or WSL2:

```sh
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure
cmake --preset tsan
cmake --build --preset tsan -j
ctest --preset tsan --output-on-failure
```

Those generic presets use the chosen compiler's default standard library.
The tested runner-default libstdc++ configurations currently fail the mandatory
resource-failure regression. For the verified Linux Clang 21/libc++ variant,
configure each fresh build with both compiler and library explicit:

```sh
cmake --preset release -DCMAKE_CXX_COMPILER=clang++-21 -DCMAKE_CXX_FLAGS=-stdlib=libc++
cmake --build --preset release --parallel 2
ctest --preset release
```

Use `debug`, `asan` and `tsan` in their own build directories for the other
configurations. The [verified workflow](https://github.com/MvsArjun/ApexMatch/blob/8337a3d91200e990ab0586859871606ffe8136e4/.github/workflows/ci.yml)
records package setup and the Linux TSan shadow-memory host setting. These
instructions require the matching Clang/libc++ installation; they do not alter
or suppress the libstdc++ regression.

ASan and UBSan are enabled together in the `asan` configuration; TSan is a
separate configuration because it cannot be combined with ASan. TSan focuses on
the queue/pipeline ownership boundary and networking tests as well as running
the rest of the suite. A sanitizer build passing is evidence for exercised
paths, not a proof that every possible execution is safe.

The local host is Windows without Linux/WSL2. Native Clang ASan/UBSan passed the full
audit suite with `detect_leaks=0`; its runtime does not support leak detection.
Use `ctest --preset asan-windows` for that native variant. Hosted Ubuntu 24.04
Clang 21/libc++ subsequently passed the full ASan/UBSan suite with Linux leak
detection enabled and the separate TSan suite. No local WSL2 run is claimed.
The hosted results do not clear the retained libstdc++ failures or establish
the absence of every possible leak or data race.

Historical initial implementation evidence: on 2026-10-18, a native Windows Clang 23 C++20 release compilation of the core
suite passed all 22 named core cases, including all 40,000 differential
requests. The replay checks also passed against the compiled CLI. This local
result is independent of the final combined extension, network, and sanitizer
verification recorded in the delivery report.
