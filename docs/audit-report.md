# Final engineering audit and release report

Audit date: 18 October 2026. Starting revision: `bf0e409`. The existing
repository was reviewed and improved in place. Working functionality and the
matching architecture were preserved. The approved public repository is now
[MvsArjun/ApexMatch](https://github.com/MvsArjun/ApexMatch), with existing history retained.

The complete implementation is present. **Verification is not fully green:**
the tested native GCC 16.2 and hosted Linux libstdc++ runtimes crash under
injected pool-resource allocation failure. Native Clang/libc++ passes the full
suite, as do four actual hosted Linux Clang 21/libc++ configurations, including
ASan/UBSan with leak detection and separate TSan. The full CI matrix remains red.

## 1. Existing issues and initial component classification

The untouched archived source rebuilt and passed its original four CTest groups.
Source inspection and expanded tests then found gaps beyond those original checks.

| Component | Initial classification | Final audit disposition |
|---|---|---|
| Limit insertion, bid/ask maintenance, maker-price executions | Verified working in exercised paths | Both sides and noncrossing behavior pass Clang tests/reference oracle. |
| Price-time priority, partial/multiple fills, level removal | Verified working in exercised paths | Original FIFO/arrival behavior preserved; no matching-rule defect found. |
| Cancellation and permanent accepted-ID registry | Verified working in exercised paths | Partial/inactive cancellation, retired IDs and rejected-ID reuse pass. |
| Event sequencing, parser and deterministic replay | Verified working for parsed inputs | Typed API enum defect corrected; exact oracle rejection reasons added. |
| Depth/active-order queries | Defective or inconsistent for invalid typed Side | Invalid values now throw rather than silently selecting sell. Normal query tests pass. |
| PMR baseline and optional pool | Implemented but exception coverage incomplete | Native and hosted Clang/libc++ failure injection passes; native/hosted libstdc++ dependency failure remains open. |
| Bounded queue/pipeline | Implemented with tested normal shutdown | Added multi-consumer/error cases; constructor ordering hardened. Generic blocked input still cannot be interrupted. |
| Local TCP server/client | Defective client bounds enforcement | Reproduced framed-response bypass; bounded input and boundary tests added. |
| Market-data publisher | Verified working in exercised paths | Serialized increments reconstruct against top/depth snapshots and BOOK. |
| Simulated TWAP | Verified working in exercised paths | Added maximum and zero-duration scheduling cases; no fill assumption. |
| Correctness/integration suite | Working, with audit coverage gaps | Expanded from 26 to 29 named C++ cases, plus 13 tooling cases. |
| Benchmark tooling | Defective evidence/provenance safeguards | Rejects inappropriate builds and evidence reuse; schema/metrics validated. |
| Sanitizer/CI configuration | Implemented, partly environment-blocked | Native ASan/UBSan and hosted libc++ ASan/UBSan/LSan/TSan pass; seven libstdc++ jobs fail. |
| Required runtime features absent | Not implemented: none found | Resume/walkthrough/practice release documents were missing and are now added. |

## 2. Corrections made

* `event_json` validated an enum only after an unchecked array index. Invalid
  typed `EventKind(99)` reproduced UBSan out-of-bounds access; it now throws.
* Invalid typed Side, AllocationMode and CommandKind values no longer silently
  choose another behavior. Invalid order-side submissions retain normal rejection.
* Sequence arithmetic was extracted into the same internal pure predicate used
  by production and boundary tests; the conservative policy did not change.
* A TCP response ending with newline could bypass the 1 MiB check. Size checks
  now precede returning a frame; receive retention is bounded.
* TCP client input used unbounded `getline` before enforcing 4096 bytes. It now
  rejects byte 4097 without waiting for newline/EOF. Socket timeout setup and
  initial response-output errors are checked.
* Pipeline engine construction now precedes producer startup, preventing a
  potential initialization-exception join of a producer blocked on full capacity.
  This is reviewed hardening, not a reproduced constructor-failure test result.
* Benchmark tooling now checks Release/optimization/sanitizer provenance,
  preserves prior evidence, validates rows/repeats/outcomes/arithmetic, uses
  strict numeric parsing and overflow-safe nearest rank, and names request
  throughput unambiguously. Metadata records portable paths and digests.
* Verification tooling records portable logs, preserves failure evidence, and
  treats zero discovered tests as failure. The empty-suite check exited 8 as expected.

No custom pool, lock-free queue, new order type or unrelated infrastructure was
added. The GCC library failure was isolated rather than hidden with a test skip.

## 3. Files modified

The exact source/document/evidence list is in [release-manifest.md](release-manifest.md).
Principal source changes are `src/engine.cpp`, `src/protocol.cpp`,
`src/sequence_capacity.hpp`, `src/extensions.cpp`, the public core headers and
`benchmarks/benchmark.cpp`. Tests and tooling were expanded in their existing
directories. CMake presets, minimum compiler diagnostics, sanitizer timeouts,
workflow permissions/time limits and ignore rules were improved.

Machine-specific paths were removed from current metadata and verification
logs. Historical numeric data were preserved; Git history was not rewritten.

## 4. New and improved tests

Three new core named cases test typed arguments, sequence limits and safe
destruction after injected book-resource failures. Differential tests now
compare exact rejection reasons on all 40,000 requests. Extension cases add
two-producer/two-consumer delivery, producer error draining, real-socket exact
and over-limit response/command boundaries, and TWAP extreme/tie schedules.

Eight benchmark-tooling cases validate evidence and metrics; five release-tooling
cases validate scanning, artifact detection and history preservation. Existing
partial-write, 600-maker sweep, malformed input, golden replay and TWAP program
checks remain enabled. Details: [core](core-audit.md), [extensions](extensions-audit.md),
[benchmark](benchmark-audit.md), and [testing](testing.md).

## 5. Build configurations tested

Fresh native Clang 23.1.3 Debug, Release and Debug ASan/UBSan; fresh native
GCC 16.2 Debug and Release. All five configured and compiled. GCC test execution
failed in its standard library. An archived original checkout was built before
changes; final clean-checkout evidence is recorded separately in verification.
Actual hosted Ubuntu 24.04 runs additionally configured, compiled and tested
GCC 13.3/libstdc++ and Clang 18.1.3/libstdc++ Debug/Release/ASan, separate
Clang/libstdc++ TSan, and Clang 21.1.8/libc++ Debug/Release/ASan/TSan.

## 6. Exact test results

| Native configuration | CTest groups passed / failed | C++ named cases | Python tooling cases |
|---|---:|---:|---:|
| Clang Debug | 6 / 0 | 29 passed | 13 passed |
| Clang Release | 6 / 0 | 29 passed | 13 passed |
| Clang ASan/UBSan | 6 / 0 | 29 passed | 13 passed |
| GCC Debug | 5 / 1 | Aborted in pool failure injection; no full pass | 13 passed |
| GCC Release | 5 / 1 | Aborted in pool failure injection; no full pass | 13 passed |
| Final clean clone, Clang Release | 6 / 0 | 29 passed | 13 passed |

Each complete C++ run includes 40,000 fixed-seed differential requests in both
allocation modes. Those requests are not additional separately named cases.
GCC's five other passing groups include replay/network/program checks as well
as tooling; they do not erase its failure. [Verification and logs](verification.md).
The final clone used committed revision `7b47fb7` and no prior build products.
It passed the expanded release-scanner assertions and the complete current suite.

Actual hosted run `39149291801` at `8337a3d` completed with:

| Linux configuration | CTest groups passed / failed |
|---|---:|
| Clang 21.1.8 / libc++ 21.1.8 Debug | 6 / 0 |
| Clang 21.1.8 / libc++ 21.1.8 Release | 6 / 0 |
| Clang 21.1.8 / libc++ 21.1.8 ASan/UBSan + leak detection | 6 / 0 |
| Clang 21.1.8 / libc++ 21.1.8 TSan | 6 / 0 |
| GCC 13.3 / libstdc++ Debug, Release, ASan (3 jobs) | Each 5 / 1 |
| Clang 18.1.3 / libstdc++ Debug, Release, ASan, TSan (4 jobs) | Each 5 / 1 |

Each complete libc++ run executes the same 29 named C++ cases, 13 tooling cases
and integration coverage. All seven failures occur in the unchanged mandatory
pool regression. [Actual job evidence](../results/hosted-ci/39149291801/summary.json).

## 7. Sanitizer results

The full six-group Clang ASan/UBSan run passed with no diagnostic, leak checking
disabled because the Windows runtime does not support it. Total was 111.33 s;
the C++ group was 80.20 s. The old invalid-enum defect was reproduced under UBSan
before correction. Native LSan and TSan are unavailable. Hosted Linux Clang
21/libc++ now passes the entire suite under ASan/UBSan with leak detection
enabled, and separately under TSan, without diagnostics. The tested TSan jobs
set `vm.mmap_rnd_bits=28`. The seven libstdc++ sanitizer/ordinary configurations
do not complete the core suite and remain failures.

The new allocation test demonstrated a GCC runtime defect. An engine-free
probe crashes with `0xc0000005`, while libc++ propagates `bad_alloc`. GDB,
source analysis, logs and reproduction are in [toolchain-issue.md](toolchain-issue.md).
The next hosted run reproduced the engine-free crash in all seven libstdc++
jobs (exit 139); all four libc++ probes propagated `bad_alloc` safely. These
probes were uninstrumented; sanitizer claims above come from the full test
builds. No standard library was patched and no upstream issue was posted.

## 8. Repeated benchmark results

The audited native Clang Release run completed **108 observations**: six
workloads, 10K/100K/1M requests, two modes, three repeats, fixed seed and warm-up.
The clean recorded source revision was `40ead65`; binaries/source hashes and
portable compiler flags are in [metadata](../results/audit-native-clang23/metadata.json).

At one million requests, median batch throughput in millions of requests/s was:

| Workload | Baseline | Pool | p99 baseline / pool (ns) |
|---|---:|---:|---:|
| Noncrossing | 1.833 | 3.074 | 800 / 700 |
| Immediate | 4.661 | 6.572 | 300 / 300 |
| Cancellation bursts | 4.018 | 5.343 | 500 / 300 |
| Shared level | 2.769 | 5.918 | 500 / 200 |
| Large book | 0.395 | 0.499 | 4,000 / 3,400 |
| Asymmetric | 1.617 | 2.777 | 1,300 / 800 |

[Full means/percentiles/memory/repeats](../results/audit-native-clang23/summary.md)
and [raw JSON](../results/audit-native-clang23/results.json) are retained.
The initial 108 observations remain separately labeled; the old headline is
not silently reused as a new result.

## 9. Performance and memory tradeoffs

The pool had higher median throughput and peak requested memory in all 18
pairs; p99 was lower in 16 and equal in two. At one-million noncrossing requests,
upstream book allocations were 3,002,040 versus 129, with 147.34 versus 199.41 MiB
peak requested memory (35.3% more). This is requested resource memory, not RSS.
The baseline stays the default. No new engine optimization was justified by
available CPU evidence; the existing optional allocator experiment was remeasured.

Clock quantization was 100 ns; median timer pairs recorded zero. Some repeat
ranges overlap materially. Neither a new causal speedup over historical code
nor production tail guarantees are established. [Analysis](performance.md).

## 10. Documentation improvements

README, design, testing, performance, extensions, verification, study guide and
interview explanations were cross-checked against source and actual evidence.
New audit reports separate reproduced defects, reviewed hardening and blocked
checks. New [resume alternatives](resume-notes.md), [walkthrough](project-walkthrough.md),
and [20 demanding model-answer questions](interview-practice.md) ground career
material in tested capabilities and measured limits. MIT was explicitly confirmed.

## 11. Git status and commits

Existing history was preserved. Focused audit commits include `8688727` (core
guards/boundaries), `42c47a7` (client/pipeline), `40d6242` (benchmark/verification),
and `40ead65` (evidence and GCC blocker). Later commits add release documentation,
fresh-clone evidence and the final scan/manifest. Approved revision `4f09688`
was pushed publicly; `8337a3d` added independent libc++ checks and hosted
evidence. Later documentation/evidence commits record the actual results.
See `git log --oneline bf0e409..HEAD`.
The release manifest identifies committed contents; build trees and compiler
downloads remain outside tracked source. The working tree is checked before handoff.

## 12. GitHub publication

The user approved public `MvsArjun/ApexMatch`, MIT and preservation of existing
history. The repository was created, pushed via authenticated Git Credential
Manager and verified through its remote refs and actual Actions results.
`origin` is `https://github.com/MvsArjun/ApexMatch.git`, default branch `master`.
Original history retains 11 old personal-path blobs as approved; current files
are redacted. See [publication record](release-preparation.md) and the scan/manifest.

## 13. CI verification

The workflow retains all seven original libstdc++ jobs and adds four Clang
21/libc++ configurations. The [actual latest source run](https://github.com/MvsArjun/ApexMatch/actions/runs/39149291801)
at `8337a3d` completed with **4 jobs passed, 7 failed**, so overall CI is red.
Each passing job ran the full six groups. Each failed job passed five groups
and crashed in the core pool regression, then independently reproduced the
engine-free standard-library crash. No failure was allowed, skipped or hidden.
actionlint 1.7.12 passed locally, with optional ShellCheck unavailable.
Later documentation/results-only pushes do not trigger another workflow run.

## 14. Outstanding limitations and blockers

* Native GCC 16.2 and seven hosted Linux libstdc++ jobs fail the pool
  resource-exhaustion regression. Exact Linux libstdc++ package version was not
  captured; compiler version alone does not identify its installed runtime.
* CPU sampling/counters are unavailable: no Linux perf; Windows WPR CPU start
  failed performance policy `0xc5585011`, with no recording left active.
* Intentional simulator limits remain: one instrument, limit orders, growing
  lifetime IDs, no transactional allocation rollback, generic blocked-input
  cancellation, bounded application TCP buffers but no exactly-once recovery,
  no durable journal/authentication/live trading or production SLA.

## 15. Recommended next improvements

First verify a corrected libstdc++ runtime using the standalone probe, then rerun
the unchanged full suite and actual Linux CI. Preserve the passing libc++ checks.
Run the supplied perf profile on a supported host. Measure under finer clock resolution and controlled
CPU conditions before interpreting small tail differences. Only measured
hotspots should motivate further allocation/event-storage work. Keep runtime
versions and the open failure visible in portfolio/interview claims.
