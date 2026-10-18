# Resume notes

These are three alternative bullets, not three independent work achievements.
Use the version appropriate to the role and adjust only facts you can explain.

## Quant Developer

* Built ApexMatch, a deterministic C++20 limit-order-book simulator implementing
  price-time priority, partial fills, cancellation and sequenced replay; validated
  correctness against an independent reference engine across 40,000 fixed-seed requests.

## C++ Trading Systems Developer

* Engineered a single-owner C++20 matching engine with bounded producer-consumer
  replay, localhost TCP order entry, market-data snapshots and simulated TWAP;
  passed 29 named C++ cases and complete Linux Clang/libc++ ASan/UBSan, leak and TSan checks.

## Low-Latency Software Engineer

* Benchmarked C++20 book allocators across six workloads and 10K–1M requests;
  measured 3.07M requests/s pooled versus 1.83M baseline on a 1M-request stream,
  with approximately 35% higher requested book memory and identical differential-test outcomes.

## Evidence and interview limits

The third bullet refers to the engineering-audit run in
[results/audit-native-clang23](../results/audit-native-clang23/summary.md),
noncrossing workload, seed 305419896, three repeats, AMD Ryzen 5 6600H,
Windows 11, Clang 23.1.3 release. Throughput is incoming engine requests per
second, not executions per second or network traffic. Peak requested book
memory was 147.34 MiB baseline versus 199.41 MiB pooled; this is not RSS.
The earlier delivery run remains separately labeled in
[performance.md](performance.md). Neither run establishes a production SLA.

The second bullet describes the tested Clang/libc++ runtime, including four
successful hosted Linux jobs. Native GCC 16.2 and seven hosted Linux libstdc++
configurations build but fail the mandatory pool allocation-failure regression;
the overall workflow remains red. Do not describe this as a fully passing
cross-compiler matrix. See
[verification.md](verification.md) and [the dependency reproducer](toolchain-issue.md).

Do not claim this is a production exchange, brokerage integration, profitable
strategy, lock-free engine, nanosecond network system, or deployed trading service.
Be ready to explain FIFO node lifetimes, permanent ID-history growth, closed-loop
benchmark limits, timer quantization, pool retention and the dependency failure.
