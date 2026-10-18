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
and every statistics counter. Rejection reason text is checked as nonempty;
the inactive-cancel policy also has an exact reason assertion. The production
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

ASan and UBSan are enabled together in the `asan` configuration; TSan is a
separate configuration because it cannot be combined with ASan. TSan focuses on
the queue/pipeline ownership boundary and networking tests as well as running
the rest of the suite. A sanitizer build passing is evidence for exercised
paths, not a proof that every possible execution is safe.

The host is Windows without Linux/WSL2. Native Clang ASan/UBSan passed the full
audit suite with `detect_leaks=0`; its runtime does not support leak detection.
Use `ctest --preset asan-windows` for that native variant. Linux/WSL2, TSan,
LeakSanitizer and hosted CI remain unverified. Linux reproduction commands and
the workflow are configuration, not evidence that those checks ran.

On 2026-10-18, a native Windows Clang 23 C++20 release compilation of the core
suite passed all 22 named core cases, including all 40,000 differential
requests. The replay checks also passed against the compiled CLI. This local
result is independent of the final combined extension, network, and sanitizer
verification recorded in the delivery report.
