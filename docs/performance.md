# Performance experiments and reproduction

The project preserves two configurations of the same matching implementation. `AllocationMode::Baseline` gives the PMR containers `std::pmr::new_delete_resource()`. `AllocationMode::Pool` gives those same containers an engine-owned `std::pmr::unsynchronized_pool_resource`. Container types, matching logic, validation, events, hash functions, and input streams stay the same. The baseline remains the application default: the experimental pool is an explicit choice, not a universal performance promise.

## Build and run

Use a release build without sanitizers. From the repository root on Linux or WSL2:

```sh
cmake --preset release
cmake --build --preset release -j
ctest --preset release
python3 tools/run_benchmarks.py --build-dir build/release --output results/benchmark --repeats 3
```

The runner detects `.exe` on Windows; use `python` there. `--executable PATH` overrides executable discovery, including for nonstandard build layouts. A compiler installed outside PATH must be available when CMake configures the build; any runtime libraries required by that compiler must also be available when the runner launches executables.

A quick check and a focused measurement:

```sh
build/release/apexmatch_bench --size 10000 --workload all --mode both --warmups 1 --repeats 1
python3 tools/run_benchmarks.py --build-dir build/release --sizes 100000 --workloads cancellation_bursts --repeats 5 --output results/cancellation
build/release/apexmatch_bench --size 100000 --workload immediate --mode baseline --format csv --output results/immediate.csv
```

The runner writes `metadata.json`, `raw.jsonl`, `raw.csv`, `results.json`, `summary.csv`, and `summary.md`. Raw records retain individual repeats; summaries report their medians and throughput range. Raw records are saved incrementally if a later workload fails. Rerunning into the same directory replaces its prior measurements; choose another output directory to preserve an experiment.

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

1. **Batch throughput:** one `steady_clock` interval surrounds all calls to `add`/`cancel`, including processing and destruction of their returned event vectors. `events_per_second = input_requests * 1e9 / batch_ns`. Here “events” means incoming add/cancel requests, not the larger number of emitted events. `emitted_events` is recorded separately. Engine construction and destruction, generation, parsing, JSON, file/console I/O, invariant checking, and percentile calculation lie outside the interval.
2. **Latency:** each request has its own pair of `steady_clock::now()` calls, again including result-vector destruction. The mean is the arithmetic mean; p50/p95/p99 use nearest rank over sorted integer nanosecond samples. No samples are discarded. A vector of samples is allocated before timing. Per-request timer calls perturb this pass, so its reciprocal mean is not the reported throughput.
3. **Allocation/memory:** one untimed pass supplies a counting `std::pmr::memory_resource` as the engine's upstream. Baseline nodes/buckets call it directly; pool chunk requests call it through the pool. It forwards both the exact size and required alignment to `new_delete_resource`. `allocations` counts these upstream allocation calls, `allocated_bytes_total` is cumulative requested bytes, `peak_live_requested_bytes` is the maximum outstanding requested bytes, and `retained_requested_bytes` is outstanding bytes after the final request and before the engine is destroyed. The scan is reused for all timing repeats of that configuration because the request stream and allocator behavior are deterministic. It verifies that tracked bytes return to zero after engine destruction.

The counters cover only allocations routed through the book resource: price-level nodes, FIFO order nodes, active-ID/history nodes, hash buckets, and pool chunks. They exclude ordinary returned event vectors, the pool resource object's own `make_unique` allocation, allocator bookkeeping, underlying heap fragmentation, process code, stacks, workload/sample storage, and OS pages. Pool upstream blocks include unused pool capacity; they are correctly part of the pool's requested memory. This is a precise requested-byte measure for the instrumented resource and an approximate comparison of book memory, **not process RSS**. It does not intercept every global allocation or imply zero allocation elsewhere.

Timing passes use the default upstream directly and contain no allocation-counter wrapper or global `new` interposition. Only the separate untimed memory pass adds counters. Both timing modes still create/destroy their ordinary result-event vectors, so this common cost can limit any benefit from optimizing book allocation.

`timer_pair_median_ns` is the median of 10,000 back-to-back clock pairs in that process. It is a scale reference, not a correction subtracted from every latency. Instruction ordering, OS scheduling, frequency changes, virtualized clocks, and coarse Windows clock granularity can affect samples. Individual samples may quantize or be zero on sufficiently coarse timers. Long tails may reflect descheduling rather than slow matching code.

Each configuration has one full-size warm-up by default and three measured repeats. Invariants run after each timing pass. The runner alternates which mode runs first across workload/size pairs and launches a separate process for each mode. This reduces systematic ordering bias without eliminating thermal, frequency, and background-workload effects. Both modes have to produce the same counts across throughput, latency, and memory passes; correctness equivalence is established more thoroughly by the automated reference-model tests.

## What the allocator experiment tests

The baseline allocates price-level nodes, FIFO nodes, active-ID nodes, accepted-ID history nodes, and hash bucket arrays through the ordinary resource. Erasing an order releases those nodes upstream immediately. The pool groups similar allocation sizes into chunks, can reuse freed nodes, and releases its chunks when the book dies. It may reduce allocator calls and improve locality, especially with churn. It can retain more unused storage and has setup/size-class costs. Event vectors deliberately use their existing standard allocator in both variants; this leaves a shared source of allocation and isolates the book allocator choice.

This changes one major aspect: allocator resource. It does not simultaneously introduce intrusive queues, flat hash maps, reserved capacities, a new price representation, or batching. Comparing multiple changes at once would obscure the cause of a difference. The pool is unsynchronized because each engine has one owner; sharing an engine concurrently would violate the design independently of allocator safety.

Accepted IDs are never reused, so `seen_` grows with historical accepted adds even when active book size returns to zero. Cancellation and immediate-fill workloads therefore retain ID-history memory. A stable active-order count is not evidence of bounded total memory. A bounded session/recovery policy is needed for a production lifetime; this simulator makes that tradeoff visible.

## Reading recorded observations

See the checked-in [benchmark report](../results/benchmark/summary.md), [raw observations](../results/benchmark/raw.csv), and [machine/build metadata](../results/benchmark/metadata.json) when present. Those files are generated by actual runs; none of the tables in this methodology document are predicted measurements. The report includes pool/baseline ratios per workload and size. Throughput ratio above 1 favors the pool; p99 or memory ratio above 1 indicates a larger recorded tail or more memory.

Do not average a p99 with a p50 or infer a pooled percentile from repeat percentiles. The summary's p99 is the median of each repeat's p99. Inspect the raw repeat spread and rerun noisy cases before calling a small change an improvement. A pool that helps cancellation bursts can still lose on a short shared-level run; neutral and negative results belong in the report.

## Profiling

On a Linux/WSL host with a kernel-compatible `perf`, from the repository root:

```sh
bash tools/profile.sh build/release noncrossing 1000000 results/profile
# Inspect results/profile/baseline-report.txt and pool-report.txt.
```

The helper captures repeated hardware counters plus call-stack samples for each mode. Kernel permissions and some virtualized machines may disallow hardware counters; if so, record the exact error and use `perf record -e cpu-clock` where permitted. Native Windows does not provide Linux `perf`. Installing a userspace binary without the needed kernel support does not verify profiling.

`--profile-only` avoids per-request latency sampling and allocation scans. Whole-process `perf` still observes generation, warm-up, invariant checks, constructors, destructors, timer calibration, and output outside the benchmark's matching interval. Filter sampled stacks to `MatchingEngine::add`/`cancel`, resource allocation, and event creation before attributing costs to the hot path. The checks themselves allocate a temporary set; they must not be confused with matching allocations.

Useful questions to investigate with measured stacks/counters:

- Does upstream allocation consume appreciable matching time, and does its share fall with the pool?
- Do tree traversal and pointer chasing remain dominant after allocator changes?
- Are cache misses/instructions correlated with large numbers of price levels rather than just active orders?
- Does event-vector growth dominate immediate-execution runs, limiting benefits from book allocation changes?
- Is a tail difference stable across repeated isolated runs or explained by scheduling noise?

These are investigation questions, not claims that a profiler found them. Any unavailable profiling or sanitizer checks must be stated in the verification report; timing results alone do not demonstrate memory or concurrency safety.
