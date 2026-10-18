# Performance experiments and reproduction

The project preserves two configurations of the same matching implementation. `AllocationMode::Baseline` gives the PMR containers `std::pmr::new_delete_resource()`. `AllocationMode::Pool` gives those same containers an engine-owned `std::pmr::unsynchronized_pool_resource`. Container types, matching logic, validation, events, hash functions, and input streams stay the same. The baseline remains the application default: the experimental pool is an explicit choice, not a universal performance promise.

[benchmark-audit.md](benchmark-audit.md) records the methodology review, evidence-handling fixes, optimization decision, and remaining interpretation limits.

## Build and run

Use a release build without sanitizers. From the repository root on Linux or WSL2:

```sh
cmake --preset release
cmake --build --preset release -j
ctest --preset release
python3 tools/run_benchmarks.py --build-dir build/release --output results/benchmark --repeats 3
```

The runner detects `.exe` on Windows; use `python` there. `--executable PATH` overrides executable discovery, including for nonstandard build layouts, and must refer to the fresh executable from the supplied build directory. The runner checks that its CMake cache says `Release`, its benchmark compile command enables optimization, and sanitizer options/instrumentation are absent. It rejects known Debug or instrumented builds. Missing cache/compile-command provenance requires the explicit `--allow-unverified-build` override and is marked unverified in metadata. This inspection cannot prove that an arbitrary executable corresponds to its cache; build the current sources immediately before measuring. A compiler installed outside PATH must be available when CMake configures the build; any runtime libraries required by that compiler must also be available when the runner launches executables.

A quick check and a focused measurement:

```sh
build/release/apexmatch_bench --size 10000 --workload all --mode both --warmups 1 --repeats 1
python3 tools/run_benchmarks.py --build-dir build/release --sizes 100000 --workloads cancellation_bursts --repeats 5 --output results/cancellation
build/release/apexmatch_bench --size 100000 --workload immediate --mode baseline --format csv --output results/immediate.csv
```

The runner writes `metadata.json`, `raw.jsonl`, `raw.csv`, `results.json`, `summary.csv`, and `summary.md`. Raw records retain individual repeats; summaries report their medians and throughput range. Raw records are saved incrementally if a later workload fails. The runner refuses a directory containing any previous report file, so failed runs cannot leave an old summary appearing to describe new partial observations. Choose a new directory for every experiment.

Metadata records the compiler version, exact compile flags, CPU/OS/RAM, seed, warm-ups, repeat order, source revision/status, and SHA-256 digests of the executable and relevant source/build/tooling files. Absolute repository, build, toolchain, and home paths use placeholders; other absolute path tokens retain their basename. These substitutions preserve flag values and repository layout while avoiding publishing a developer's local directory names. Digests identify the captured files; they do not by themselves prove executable/source correspondence.

## Workload definitions

All generation occurs before timing. The default seed is decimal `305419896`. A specified SplitMix64 implementation and modulo reduction avoid depending on implementation-specific `uniform_int_distribution` or shuffle behavior. Each configuration processes exactly the requested number of input requests. Odd lengths or lengths not divisible by a burst size can end in the middle of a pair or burst.

| Name | Input and purpose |
|---|---|
| `noncrossing` | About half buys at 98,001–99,000 ticks and half sells at 101,000–101,999 ticks; random quantities 1–100. Many live orders across at most 2,000 levels. |
| `immediate` | Alternating sell maker and equally sized buy taker, both 100,000 ticks and quantity 10. Every second request immediately removes its preceding maker. |
| `cancellation_bursts` | Up to 256 noncrossing adds, followed by cancellations of those IDs in a fixed-seed shuffled order. Measures node churn and cancellation lookup. |
| `shared_level` | Every request buys at 100,000 ticks, quantity 1–100. One long FIFO and growing ID indexes. |
| `large_book` | Each request has a distinct price level; buys below/equal 1,000,000,000 and sells at/above 2,000,000,000. A fixed-seed shuffle varies insertion order without crossing. Stresses tree nodes and retained memory. |
| `asymmetric` | Roughly 90% buys at 99,000–99,999, 10% sells at 99,500; quantities 1–100. Some aggressive sells match high bids while low bids accumulate. |

The complete matrix uses 10,000, 100,000, and 1,000,000 requests, all six workloads, and both allocation modes. The million-order large-book case intentionally retains a large working set; ensure sufficient RAM and avoid swapping. The generator's memory and the latency sample array remain in the process but are excluded from the allocation scan.

## Measurement boundaries

Three distinct passes use a fresh engine and the identical pre-generated stream:

1. **Batch throughput:** one `steady_clock` interval surrounds all calls to `add`/`cancel`, including processing and destruction of their returned event vectors. Schema 2 names this metric `requests_per_second = input_requests * 1e9 / batch_ns`. Historical schema 1 called the same incoming-request metric `events_per_second`; it never measured the larger number of emitted events. `emitted_events` is recorded separately. Engine construction and destruction, generation, parsing, JSON, file/console I/O, invariant checking, and percentile calculation lie outside the interval.
2. **Latency:** each request has its own pair of `steady_clock::now()` calls, again including result-vector destruction. The mean is the arithmetic mean; p50/p95/p99 use nearest rank over sorted integer nanosecond samples. No samples are discarded. A vector of samples is allocated before timing. Per-request timer calls perturb this pass, so its reciprocal mean is not the reported throughput.
3. **Allocation/memory:** one untimed pass supplies a counting `std::pmr::memory_resource` as the engine's upstream. Baseline nodes/buckets call it directly; pool chunk requests call it through the pool. It forwards both the exact size and required alignment to `new_delete_resource`. `allocations` counts these upstream allocation calls, `allocated_bytes_total` is cumulative requested bytes, `peak_live_requested_bytes` is the maximum outstanding requested bytes, and `retained_requested_bytes` is outstanding bytes after the final request and before the engine is destroyed. The scan is reused for all timing repeats of that configuration because the request stream and allocator behavior are deterministic. It verifies that tracked bytes return to zero after engine destruction.

The counters cover only allocations routed through the book resource: price-level nodes, FIFO order nodes, active-ID/history nodes, hash buckets, and pool chunks. They exclude ordinary returned event vectors, the pool resource object's own `make_unique` allocation, allocator bookkeeping, underlying heap fragmentation, process code, stacks, workload/sample storage, and OS pages. Pool upstream blocks include unused pool capacity; they are correctly part of the pool's requested memory. This is a precise requested-byte measure for the instrumented resource and an approximate comparison of book memory, **not process RSS**. It does not intercept every global allocation or imply zero allocation elsewhere.

Timing passes use the default upstream directly and contain no allocation-counter wrapper or global `new` interposition. Only the separate untimed memory pass adds counters. Both timing modes still create/destroy their ordinary result-event vectors, so this common cost can limit any benefit from optimizing book allocation.

`timer_pair_median_ns` is the median of 10,000 back-to-back clock pairs in that process. It is a scale reference, not a correction subtracted from every latency. Instruction ordering, OS scheduling, frequency changes, virtualized clocks, and coarse Windows clock granularity can affect samples. Individual samples may quantize or be zero on sufficiently coarse timers. Long tails may reflect descheduling rather than slow matching code.

Each configuration has one full-size warm-up by default and three measured repeats. Every pass constructs a fresh empty engine; warm-up exercises allocator/code/OS caches, rather than preloading an engine retained for measurement. The untimed allocation pass also precedes measured repeats. Invariants run after each timing pass. The runner alternates which mode runs first across workload/size pairs and launches a separate process for each mode. This reduces systematic ordering bias without eliminating thermal, frequency, and background-workload effects. Both modes have to produce the same counts across throughput, latency, and memory passes; correctness equivalence is established more thoroughly by the automated reference-model tests. The runner also validates repeat uniqueness, configuration fields, finite nonnegative metrics, throughput arithmetic, percentile ordering, memory bounds, and deterministic allocation/outcome counts. These consistency checks detect corrupt evidence, rather than prove full event-by-event equivalence.

## What the allocator experiment tests

The baseline allocates price-level nodes, FIFO nodes, active-ID nodes, accepted-ID history nodes, and hash bucket arrays through the ordinary resource. Erasing an order releases those nodes upstream immediately. The pool groups similar allocation sizes into chunks, can reuse freed nodes, and releases its chunks when the book dies. It may reduce allocator calls and improve locality, especially with churn. It can retain more unused storage and has setup/size-class costs. Event vectors deliberately use their existing standard allocator in both variants; this leaves a shared source of allocation and isolates the book allocator choice.

This changes one major aspect: allocator resource. It does not simultaneously introduce intrusive queues, flat hash maps, reserved capacities, a new price representation, or batching. Comparing multiple changes at once would obscure the cause of a difference. The pool is unsynchronized because each engine has one owner; sharing an engine concurrently would violate the design independently of allocator safety.

Accepted IDs are never reused, so `seen_` grows with historical accepted adds even when active book size returns to zero. Cancellation and immediate-fill workloads therefore retain ID-history memory. A stable active-order count is not evidence of bounded total memory. A bounded session/recovery policy is needed for a production lifetime; this simulator makes that tradeoff visible.

Allocator reliability also depends on the standard-library runtime. Final native GCC 16.2 Debug/Release testing found a pool-resource segmentation fault during injected upstream allocation failure, reproduced by a standalone standard-library-only program. The mandatory test remains enabled; those full GCC runs failed. Normal-allocation timing observations below use native Clang/libc++, and do not establish pool behavior during exhaustion or validate another runtime. See [core-audit.md](core-audit.md) for the reproducer and [verification.md](verification.md) for build outcomes.

## Audited observations: native Clang 23 on Windows

The final audited matrix completed on 18 October 2026 with **108 observations**: six workloads, 10,000/100,000/1,000,000 requests, both allocation modes, and three repeats. Each configuration used one full-size warm-up and seed `305419896`. The source tree was clean at recorded revision `40ead657caf78a7d360de97d911feec79cc53c57` when metadata was collected. The runner recorded executable/source SHA-256 digests and inspected the optimized Release compile command.

The host was an AMD Ryzen 5 6600H, 12 logical CPUs, 16,312,721,408 bytes of physical RAM, Windows 11 build 26300, and Clang 23.1.3 targeting `x86_64-w64-windows-gnu` with libc++. Compilation used C++20, `-O3 -DNDEBUG`, and no sanitizer instrumentation. No CPU affinity/isolation or fixed-frequency guarantee was applied. See the [portable metadata](../results/audit-native-clang23/metadata.json), [full report](../results/audit-native-clang23/summary.md), [raw observations](../results/audit-native-clang23/raw.csv), and [summary CSV](../results/audit-native-clang23/summary.csv).

At one million incoming requests, the median observations were:

| Workload | Throughput, baseline → pool (million requests/s) | p99, baseline → pool (ns) | Peak book requested memory, baseline → pool (MiB) | Upstream allocations, baseline → pool |
|---|---:|---:|---:|---:|
| Noncrossing | 1.833 → 3.074 | 800 → 700 | 147.34 → 199.41 | 3,002,040 → 129 |
| Immediate execution | 4.661 → 6.572 | 300 → 300 | 37.68 → 56.14 | 2,500,021 → 62 |
| Cancellation bursts | 4.018 → 5.343 | 500 → 300 | 18.86 → 32.57 | 1,969,455 → 75 |
| Shared level | 2.769 → 5.918 | 500 → 200 | 147.19 → 199.16 | 3,000,041 → 114 |
| Large book | 0.395 → 0.499 | 4,000 → 3,400 | 223.48 → 332.75 | 4,000,040 → 157 |
| Asymmetric | 1.617 → 2.777 | 1,300 → 800 | 121.06 → 166.91 | 2,886,545 → 124 |

The pool/baseline ratio of median batch throughput was:

| Workload | 10,000 requests | 100,000 requests | 1,000,000 requests |
|---|---:|---:|---:|
| Noncrossing | 1.204× | 1.531× | 1.677× |
| Immediate execution | 1.175× | 1.384× | 1.410× |
| Cancellation bursts | 1.212× | 1.307× | 1.330× |
| Shared level | 1.232× | 1.610× | 2.137× |
| Large book | 1.028× | 1.223× | 1.265× |
| Asymmetric | 1.177× | 1.494× | 1.717× |

The pool had a higher median throughput in all 18 workload/size conditions and higher peak requested memory in all 18. Median repeat p99 was lower in 16 conditions and equal in two: 10,000 asymmetric and one-million immediate. These are observed comparisons of the two allocation modes, not a significance claim. The 10,000 large-book throughput difference was only 2.8%, with overlapping repeat ranges; it is especially weak evidence of a durable advantage.

No new matching optimization was introduced in this audit. The new pool/baseline comparison re-evaluates the existing allocator choice after correctness and tooling changes. Allocation counts, peak/retained requested bytes, and active-order counts matched the historical matrix exactly in all 36 workload/size/mode configurations. At one-million noncrossing requests, the pool increased peak requested memory by about 52.07 MiB (35.3%) for the observed 1.677× median throughput. Throughput excludes engine construction/destruction time. Requested book memory includes resource setup but excludes process RSS and ordinary event-vector allocation. The baseline remains the default.

Run-to-run variation prevents attributing differences from the original revision to an audit fix. For example, one-million noncrossing baseline throughput ranged from 1.676 to 2.279 million requests/s across repeats and pool throughput from 2.459 to 4.118 million. Asymmetric baseline ranged from 1.471 to 2.234 million and pool from 2.664 to 3.503 million. The new asymmetric pool median was 2.777 million versus the historical 3.497 million, while their repeat ranges overlapped. These measurements neither isolate an optimization nor establish a causal regression. New one-million large-book medians were 0.395/0.499 million versus historical 0.422/0.539 million; its new baseline/pool p99 was 4,000/3,400 ns versus historical 3,200/3,200 ns. Current results take precedence over the older headline figures.

Every process again recorded a **0 ns** median back-to-back timer pair, and latency percentiles remained quantized at 100 ns on this host. No clock-cost correction, CPU sampling, cache-miss attribution, Linux result, or production latency guarantee is inferred from this run. All observations passed the runner's configuration/count/arithmetic checks and each timed engine pass's invariant checks; matching correctness and allocator-failure coverage remain separately documented in [testing.md](testing.md) and [verification.md](verification.md).

## Historical observations: native Clang 23 on Windows

The original matrix ran successfully on 18 October 2026: 6 workloads × 3 sizes × 2 modes × 3 repeats = **108 recorded observations**, with one full-size warm-up per mode/configuration. The host was an AMD Ryzen 5 6600H, 12 logical CPUs, 16,312,721,408 bytes of physical RAM (about 15.19 GiB), Windows 11 build 26300, and Clang 23.1.3 targeting `x86_64-w64-windows-gnu`. The release compile used C++20, `-O3 -DNDEBUG`, with sanitizers off. Recorded source revision: `21000218f16078c20f5b3271393035033436fa34`. Flags, seed, invocation, and CPU details are in [metadata.json](../results/native-clang23/metadata.json). These are historical observations of that revision, rather than a promise about the audited revision.

The [full generated report](../results/native-clang23/summary.md), [108 raw observations](../results/native-clang23/raw.csv), [summary CSV](../results/native-clang23/summary.csv), and [combined JSON](../results/native-clang23/results.json) are the evidence for the following analysis. The baseline/pool request and outcome counts agreed. Each timed pass completed its post-timing invariant check; the separate memory scans returned to zero tracked bytes after destruction.

Median batch throughput improved with the pool in all 18 workload/size conditions on this host. The ratio below is pool divided by baseline; this is an observation of these runs, not a guarantee on other standard libraries or machines.

| Workload | 10,000 requests | 100,000 requests | 1,000,000 requests |
|---|---:|---:|---:|
| Noncrossing | 1.181× | 1.480× | 1.665× |
| Immediate execution | 1.556× | 1.351× | 1.416× |
| Cancellation bursts | 1.930× | 1.366× | 1.343× |
| Shared level | 1.201× | 1.662× | 2.079× |
| Large book | 1.203× | 1.250× | 1.277× |
| Asymmetric | 1.082× | 1.643× | 2.187× |

At one million requests, the absolute observations were:

| Workload | Throughput, baseline → pool (million requests/s) | p99, baseline → pool (ns) | Peak book requested memory, baseline → pool (MiB) | Upstream allocations, baseline → pool |
|---|---:|---:|---:|---:|
| Noncrossing | 1.839 → 3.062 | 900 → 600 | 147.34 → 199.41 | 3,002,040 → 129 |
| Immediate execution | 4.719 → 6.682 | 400 → 300 | 37.68 → 56.14 | 2,500,021 → 62 |
| Cancellation bursts | 4.047 → 5.437 | 600 → 400 | 18.86 → 32.57 | 1,969,455 → 75 |
| Shared level | 2.885 → 5.998 | 500 → 200 | 147.19 → 199.16 | 3,000,041 → 114 |
| Large book | 0.422 → 0.539 | 3,200 → 3,200 | 223.48 → 332.75 | 4,000,040 → 157 |
| Asymmetric | 1.599 → 3.497 | 1,100 → 900 | 121.06 → 166.91 | 2,886,545 → 124 |

The allocation census directly establishes that chunking/reuse reduced calls to the book's upstream resource. It does not establish a measured cache-miss reduction or identify the percentage of matching CPU time spent in allocation. The million-level workload was much slower than a million orders at one level in both modes, consistent with a larger tree/working set and more per-level work; that is an interpretation of the workload contrast, not a sampled-profiler finding.

Memory is the consistent downside: pool peak requested bytes were higher in **all 18 conditions**. Peak pool/baseline memory ratios for all sizes were:

| Workload | 10,000 requests | 100,000 requests | 1,000,000 requests |
|---|---:|---:|---:|
| Noncrossing | 3.877× | 1.930× | 1.353× |
| Immediate execution | 13.226× | 3.852× | 1.490× |
| Cancellation bursts | 10.483× | 6.342× | 1.727× |
| Shared level | 4.114× | 1.933× | 1.353× |
| Large book | 3.237× | 1.914× | 1.489× |
| Asymmetric | 4.400× | 2.024× | 1.379× |

Small workloads pay especially large relative chunk/setup retention costs: at 10,000 immediate requests, the baseline peaked near 0.33 MiB versus 4.33 MiB for the pool despite ending with no active orders. At one million immediate requests, the baseline still retained 37,169,912 requested bytes after processing, reflecting accepted-ID history; an empty active book does not mean empty memory. For cancellation bursts the exact final active counts were 240, 160, and 64 at the three sizes because each stream ends partway through its last burst.

Tail results were mostly favorable but not universal. The pool's median repeat p99 was lower in 15 conditions, equal in two (10,000 shared-level and one-million large-book), and worse in one: **10,000 large-book p99 rose from 800 to 900 ns**, despite 1.203× throughput. At one-million large-book, both p95 and p99 were unchanged at 2,500/3,200 ns, and p50 rose from 1,500 to 1,600 ns. The full report retains mean, p50, p95, and p99 for every condition; throughput improvements do not erase neutral or negative tail results.

Two measurement limits matter particularly on this host. First, the median back-to-back clock pair was recorded as **0 ns**, while displayed latency percentiles were quantized in 100 ns increments. That does not mean clock calls have zero cost; timer granularity prevents resolving some differences. Second, some three-repeat spreads were substantial: one-million large-book baseline throughput ranged from about 0.361 to 0.535 million requests/s, and asymmetric pool throughput ranged from about 2.226 to 3.746 million requests/s. These are ordinary laptop measurements without CPU isolation or a statistical significance claim. Inspect the raw repeats and rerun noisy cases before interpreting a small change as durable.

The experiment is retained as an opt-in configuration because this measured throughput benefit is useful and reproducible, while the **baseline stays the default** because the pool increases memory everywhere and does not improve every latency percentile. No other container or lookup optimization was combined with the resource change. Linux `perf` was unavailable on the native Windows verification host, so this delivery records actual allocation/memory profiling observations and timings but **does not claim sampled call stacks, hardware counters, or measured cache behavior**. The following helper makes that additional Linux investigation reproducible.

Do not average a p99 with a p50 or infer a pooled percentile from repeat percentiles. The summary's p99 is the median of each repeat's p99. Allocation ratios displayed as `0.000` in the generated report are rounded small positive values, not zero allocations; the absolute counts above and in the raw data remain authoritative.

## Profiling

On a Linux/WSL host with a kernel-compatible `perf`, from the repository root:

```sh
cmake --preset profile
cmake --build --preset profile -j
bash tools/profile.sh build/profile noncrossing 1000000 results/profile
# Inspect results/profile/baseline-report.txt and pool-report.txt.
```

The profile preset retains release optimization while adding debug symbols and frame pointers (`-O3 -DNDEBUG -g -fno-omit-frame-pointer`), making sampled call stacks useful without changing the release timing experiment. The helper defaults to `build/profile` and captures repeated hardware counters plus call-stack samples for each mode. Kernel permissions and some virtualized machines may disallow hardware counters; if so, record the exact error and use `perf record -e cpu-clock` where permitted. Native Windows does not provide Linux `perf`. Installing a userspace binary without the needed kernel support does not verify profiling.

`--profile-only` avoids per-request latency sampling and allocation scans. Whole-process `perf` still observes generation, warm-up, invariant checks, constructors, destructors, timer calibration, and output outside the benchmark's matching interval. Filter sampled stacks to `MatchingEngine::add`/`cancel`, resource allocation, and event creation before attributing costs to the hot path. The checks themselves allocate a temporary set; they must not be confused with matching allocations.

During the native Windows audit, Windows Performance Recorder was available and reported no active recording. Starting its CPU profile failed with host performance-policy error `0xc5585011`; no recording remained active. No ETW CPU samples or symbolized Windows call stacks were collected. Allocation census observations are available, while CPU attribution, hardware counters, and cache behavior remain unverified.

Useful questions to investigate with measured stacks/counters:

- Does upstream allocation consume appreciable matching time, and does its share fall with the pool?
- Do tree traversal and pointer chasing remain dominant after allocator changes?
- Are cache misses/instructions correlated with large numbers of price levels rather than just active orders?
- Does event-vector growth dominate immediate-execution runs, limiting benefits from book allocation changes?
- Is a tail difference stable across repeated isolated runs or explained by scheduling noise?

These are investigation questions, not claims that a profiler found them. Any unavailable profiling or sanitizer checks must be stated in the verification report; timing results alone do not demonstrate memory or concurrency safety.
