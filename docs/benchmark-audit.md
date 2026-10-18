# Benchmark engineering audit

The benchmark measures a single-owner matching-engine service loop over synthetic, pre-generated requests. It does not measure exchange latency, network throughput, parsing, producer/consumer queueing, or a sustained external arrival process. Historical observations are retained in `results/native-clang23`; audited observations use a separate directory. No new matching-engine optimization is justified solely by relabeling metrics or making the runner stricter.

## Inspection and corrections

| Area | Audit finding | Disposition |
|---|---|---|
| Release configuration | Original runner recorded build metadata but would accept a Debug or sanitized binary. | Runner now rejects known non-Release/instrumented configurations and effective `-O0`; missing optimized compile-command provenance needs an explicit override recorded as unverified. |
| Throughput | Arithmetic was correct; `events_per_second` actually counted incoming requests. | Schema 2 uses `requests_per_second` and `median_requests_per_second`; historical schema 1 remains interpretable without changing its numbers. |
| Evidence preservation | Reusing an output directory overwrote raw files. A failed rerun could leave a stale summary from an earlier run. | Runner refuses any destination already containing a report artifact. Partial raw observations remain identifiable and cannot be mixed with an earlier summary. |
| Provenance/privacy | Raw compiler, command, executable, and workspace paths disclosed local directory names. A revision alone did not identify uncommitted source. | New metadata substitutes path prefixes, keeps compiler basename/version/flags and relative layout, and hashes the executable plus source/build/tooling files. A fresh build remains necessary to establish correspondence. |
| Observation validation | Original runner checked row count/configuration and four end-state counters. It did not reject duplicate repeat indices or inconsistent numeric metrics. | Added repeat-index, schema, configuration, finite-value, throughput-arithmetic, percentile-ordering, memory-bound, and repeat-consistency validation. |
| Percentiles/arguments | Nearest-rank calculation was correct for the verified 64-bit runs, but `size * percent` could overflow on 32-bit `size_t`. Cast-before-range-check numeric parsing could also wrap there. | Equivalent quotient/remainder rank calculation avoids that intermediate; decimal parsing rejects invalid forms/overflow before conversion to `size_t`. No historical 64-bit numeric result was changed. |
| Timing boundaries | Separate batch and per-request passes, pre-generation, and post-timing checks are appropriate. | Retained. Returned result destruction is included consistently. |
| Allocation measurements | Counting only the PMR upstream is precise for requested book memory, but excludes event vectors and allocator/OS overhead. | Retained and labeled explicitly; never described as process RSS or total allocations. |
| Workload fairness | Fixed SplitMix64 streams and equal request counts give both allocator modes identical input. | Retained; runner alternates mode-first order between configurations. |
| Profiling | Local allocation census is available; Linux stack/counter profiling was not run. Windows recording was denied by host policy. | Report blocker explicitly; do not infer measured cache misses or allocator CPU percentages. |

Eight `unittest` methods in [benchmark_tooling_test.py](../tests/benchmark_tooling_test.py) cover acceptance of valid records, missing build provenance, rejection of Debug/sanitized/unoptimized builds, preservation of existing evidence, private-path substitution, repeat integrity, corrupt metrics, incorrect configurations, outcome drift, and median summaries. These checks run as the `benchmark_tooling` CTest group. They test the evidence-handling defects rather than re-testing matching behavior.

## What one observation includes

Each workload/mode has a default full-size warm-up, one untimed allocation scan, then three independently timed throughput/latency pairs. Every pass starts with a newly constructed empty engine. The input vector is identical, and each measured pass performs an invariant check after its timer stops.

- Batch throughput divides the exact number of incoming add/cancel requests by one elapsed interval. It includes validation, matching, event-vector creation/destruction, and a small event-count accumulation. It excludes construction/destruction of the engine, parsing, serialization, generation, invariant checking, and output.
- Per-request samples include one request and destruction of its returned events between two clock calls. p50/p95/p99 use nearest rank. The summary takes the median of repeat percentiles; it does not claim to compute a percentile over pooled raw latency samples.
- The memory scan counts upstream PMR calls/bytes including pool chunks. Peak is maximum requested bytes outstanding; retained memory is outstanding after the last request and before engine destruction. The tracked live count must return to zero after destruction. The same deterministic census is reused across timing repeats.

This tests service work from an empty book to a workload-dependent final state. It is not a warmed, fixed-size steady-state book. `shared_level` and `large_book` grow throughout the interval. `immediate` and cancellation workloads keep few active orders but retain accepted-ID history because IDs cannot be reused. Both history growth and the resulting hash rehashes are part of this API's measured behavior.

## Limits on interpretation

The Windows clock produced quantized samples in the original run, including a zero median back-to-back timer-pair measurement. Zero does not establish free clock reads. Per-request clock overhead perturbs latency, so its reciprocal mean is not the separately measured batch throughput. No correction is subtracted.

The host has no isolated core, fixed-frequency guarantee, or controlled background workload. Three repeats show an observed range, not a confidence interval or statistical significance. Mode-order alternation reduces one systematic bias but does not eliminate thermal or scheduler effects. Independent processes also have distinct heap histories and address layouts. Treat small differences and noisy tails cautiously.

End-state counters and invariants do not prove event-by-event equivalence between modes. The independent reference-model and event tests supply correctness evidence. The benchmark's synthetic streams omit malformed requests, duplicate IDs, extreme integer boundaries, a dedicated deep-sweep stress case, many realistic order-flow distributions, and cancellation of a particular partially filled maker. The asymmetric stream can naturally produce multiple fills but does not control sweep depth. Those are correctness-test concerns and possible future workload extensions, rather than claims covered by this performance matrix.

The baseline and pool share event-vector allocation and matching logic. The pool can reduce book upstream calls while increasing unused retained capacity; it cannot remove ordinary event allocation or tree/hash work. Pool setup and teardown cost are outside the measured service interval, so no total-engine-lifecycle speedup is claimed. Requested-byte accounting is not a sampled heap/RSS profile and does not establish better cache locality.

## Optimization decision

The existing opt-in PMR pool is the only allocator experiment. In the historical one-million noncrossing stream it measured approximately 1.839 million requests/s baseline and 3.062 million pool, with peak requested memory increasing from 147.34 to 199.41 MiB. Historical pool throughput improved in all 18 workload/size conditions, while memory increased in all 18 and p99 did not improve universally. Those observations support retaining the option and retaining the baseline default; they do not establish a universal pool advantage.

No intrusive container, alternative hash table, pre-reservation strategy, event batching, or custom allocator was added during this audit. There is no measured call-stack evidence yet that would justify their complexity. The audit's parser/enum/concurrency corrections and tooling changes are correctness/reproducibility work. New-run differences from the historical revision must not be presented as an optimization's causal speedup.

The final native GCC 16.2 runs uncovered a pool-resource crash under injected upstream allocation failure, independently reproduced using only the standard library. Hosted Linux libstdc++ jobs later reproduced both crashes, while four Clang 21/libc++ configurations passed the unchanged complete suite. The mandatory regression remains enabled and the overall CI workflow is red. This is separate from the normal-allocation Clang/libc++ performance experiment; it limits portability/reliability claims about the optional pool. See [core-audit.md](core-audit.md).

## Profiling availability

Linux `perf` and its kernel counters were unavailable on this native Windows host. The installed `wpr.exe` could list CPU profiles and report status. An attempted `wpr -start CPU -filemode` failed with performance-policy error `0xc5585011`; status remained not recording. No ETW CPU samples or symbolized Windows call-stack analysis were collected. The available LLVM XRay runtimes were Linux archives, and no `llvm-xray` analysis executable was found. These are environment/tool limitations, not completed profiling.

[profile.sh](../tools/profile.sh) provides optimized Linux/WSL `perf stat` and call-stack capture. It observes the whole process, including untimed generation, warm-up, checks, and destruction; filtering matching stacks is required before attributing a whole-process percentage to the timed engine loop. See [performance.md](performance.md) for commands, workload definitions, and recorded measurements.

## Completed audited matrix

The final native Clang 23 Release matrix completed with **108 observations**, covering all six workloads at all three requested sizes, both modes, and three repeats. Metadata records clean source revision `40ead657caf78a7d360de97d911feec79cc53c57`, `-O3 -DNDEBUG`, sanitizers disabled, fixed seed `305419896`, and one full-size warm-up. [Raw data and report](../results/audit-native-clang23/summary.md) preserve this run separately from the original 108 observations.

At one-million noncrossing requests the baseline/pool medians were **1.833/3.074 million requests/s**, **800/700 ns p99**, and **147.34/199.41 MiB peak requested memory**. The observed throughput ratio was 1.677× with 35.3% more peak requested memory. All 18 workload/size pairs had higher pool median throughput and higher pool memory; p99 was lower in 16 pairs and equal in two. Allocation/memory and active-order counts exactly matched the old matrix in all 36 configurations. No newly introduced optimization is responsible for these allocator-mode differences.

Noise remains material: million-request noncrossing pool throughput spanned 2.459–4.118 million requests/s, and asymmetric pool throughput spanned 2.664–3.503 million. The new asymmetric pool median fell from the historical 3.497 to 2.777 million; the historical and current repeat ranges overlap. No causal performance gain or regression from a source change is established. All processes reported a zero median timer-pair duration, which reflects the clock limitation rather than zero instrumentation cost. [performance.md](performance.md) supplies the complete current million-request table, all throughput ratios, historical context, and measurement limits.
