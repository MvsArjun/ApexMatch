# Recorded benchmark observations

Recorded: 2026-10-18T16:43:40.555593+00:00
CPU: AMD Ryzen 5 6600H with Radeon Graphics; OS: Windows-11-10.0.26300-SP0

Throughput is the median of repeated batch measurements. Latency columns are medians of per-repeat percentiles, not percentiles of pooled samples.

| Workload | Requests | Mode | requests/s | mean ns | p50 ns | p95 ns | p99 ns | allocations | peak requested MiB |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| noncrossing | 10,000 | baseline | 3,095,592 | 368 | 300 | 400 | 500 | 32,013 | 1.57 |
| noncrossing | 10,000 | pool | 3,725,644 | 298 | 200 | 300 | 300 | 75 | 6.08 |
| immediate | 10,000 | pool | 6,044,853 | 187 | 200 | 200 | 200 | 34 | 4.33 |
| immediate | 10,000 | baseline | 5,142,710 | 221 | 200 | 300 | 300 | 25,014 | 0.33 |
| cancellation_bursts | 10,000 | baseline | 3,965,107 | 280 | 200 | 400 | 600 | 20,196 | 0.21 |
| cancellation_bursts | 10,000 | pool | 4,806,768 | 233 | 200 | 300 | 400 | 47 | 2.24 |
| shared_level | 10,000 | pool | 5,436,260 | 235 | 100 | 200 | 400 | 60 | 5.83 |
| shared_level | 10,000 | baseline | 4,414,037 | 275 | 200 | 300 | 500 | 30,027 | 1.42 |
| large_book | 10,000 | baseline | 2,128,520 | 505 | 400 | 500 | 900 | 40,026 | 2.18 |
| large_book | 10,000 | pool | 2,187,466 | 469 | 300 | 500 | 800 | 82 | 7.06 |
| asymmetric | 10,000 | pool | 3,576,922 | 322 | 200 | 300 | 800 | 71 | 5.65 |
| asymmetric | 10,000 | baseline | 3,040,161 | 377 | 300 | 400 | 800 | 29,847 | 1.28 |
| noncrossing | 100,000 | baseline | 2,750,018 | 385 | 300 | 400 | 600 | 302,032 | 13.93 |
| noncrossing | 100,000 | pool | 4,209,162 | 265 | 200 | 300 | 400 | 100 | 26.88 |
| immediate | 100,000 | pool | 6,938,133 | 169 | 200 | 200 | 200 | 47 | 11.84 |
| immediate | 100,000 | baseline | 5,013,009 | 230 | 200 | 300 | 400 | 250,017 | 3.07 |
| cancellation_bursts | 100,000 | baseline | 4,043,623 | 270 | 200 | 400 | 500 | 197,247 | 1.58 |
| cancellation_bursts | 100,000 | pool | 5,286,278 | 214 | 200 | 300 | 300 | 60 | 10.03 |
| shared_level | 100,000 | pool | 6,498,655 | 181 | 100 | 200 | 200 | 85 | 26.63 |
| shared_level | 100,000 | baseline | 4,036,979 | 284 | 200 | 200 | 400 | 300,033 | 13.78 |
| large_book | 100,000 | baseline | 1,093,605 | 907 | 700 | 1,300 | 1,800 | 400,032 | 21.41 |
| large_book | 100,000 | pool | 1,337,664 | 863 | 700 | 1,200 | 1,600 | 118 | 40.97 |
| asymmetric | 100,000 | pool | 4,046,355 | 268 | 200 | 300 | 500 | 96 | 23.96 |
| asymmetric | 100,000 | baseline | 2,707,767 | 425 | 300 | 600 | 900 | 289,680 | 11.83 |
| noncrossing | 1,000,000 | baseline | 1,833,152 | 618 | 400 | 700 | 800 | 3,002,040 | 147.34 |
| noncrossing | 1,000,000 | pool | 3,074,061 | 397 | 300 | 500 | 700 | 129 | 199.41 |
| immediate | 1,000,000 | pool | 6,572,310 | 176 | 200 | 200 | 300 | 62 | 56.14 |
| immediate | 1,000,000 | baseline | 4,661,007 | 239 | 200 | 300 | 300 | 2,500,021 | 37.68 |
| cancellation_bursts | 1,000,000 | baseline | 4,017,931 | 271 | 200 | 400 | 500 | 1,969,455 | 18.86 |
| cancellation_bursts | 1,000,000 | pool | 5,343,125 | 209 | 200 | 300 | 300 | 75 | 32.57 |
| shared_level | 1,000,000 | pool | 5,917,969 | 191 | 100 | 200 | 200 | 114 | 199.16 |
| shared_level | 1,000,000 | baseline | 2,769,160 | 486 | 300 | 400 | 500 | 3,000,041 | 147.19 |
| large_book | 1,000,000 | baseline | 394,824 | 2,537 | 2,000 | 3,200 | 4,000 | 4,000,040 | 223.48 |
| large_book | 1,000,000 | pool | 499,490 | 2,070 | 1,700 | 2,700 | 3,400 | 157 | 332.75 |
| asymmetric | 1,000,000 | pool | 2,777,151 | 411 | 300 | 600 | 800 | 124 | 166.91 |
| asymmetric | 1,000,000 | baseline | 1,617,252 | 716 | 500 | 900 | 1,300 | 2,886,545 | 121.06 |

## Pool / baseline ratios

A throughput ratio above 1 favors the pool; a memory or p99 ratio above 1 means the pool used more memory or had a higher recorded tail.

| Workload | Requests | throughput ratio | p99 ratio | allocation-count ratio | peak memory ratio |
|---|---:|---:|---:|---:|---:|
| noncrossing | 10,000 | 1.204 | 0.600 | 0.002 | 3.877 |
| immediate | 10,000 | 1.175 | 0.667 | 0.001 | 13.226 |
| cancellation_bursts | 10,000 | 1.212 | 0.667 | 0.002 | 10.483 |
| shared_level | 10,000 | 1.232 | 0.800 | 0.002 | 4.114 |
| large_book | 10,000 | 1.028 | 0.889 | 0.002 | 3.237 |
| asymmetric | 10,000 | 1.177 | 1.000 | 0.002 | 4.400 |
| noncrossing | 100,000 | 1.531 | 0.667 | 0.000 | 1.930 |
| immediate | 100,000 | 1.384 | 0.500 | 0.000 | 3.852 |
| cancellation_bursts | 100,000 | 1.307 | 0.600 | 0.000 | 6.342 |
| shared_level | 100,000 | 1.610 | 0.500 | 0.000 | 1.933 |
| large_book | 100,000 | 1.223 | 0.889 | 0.000 | 1.914 |
| asymmetric | 100,000 | 1.494 | 0.556 | 0.000 | 2.024 |
| noncrossing | 1,000,000 | 1.677 | 0.875 | 0.000 | 1.353 |
| immediate | 1,000,000 | 1.410 | 1.000 | 0.000 | 1.490 |
| cancellation_bursts | 1,000,000 | 1.330 | 0.600 | 0.000 | 1.727 |
| shared_level | 1,000,000 | 2.137 | 0.400 | 0.000 | 1.353 |
| large_book | 1,000,000 | 1.265 | 0.850 | 0.000 | 1.489 |
| asymmetric | 1,000,000 | 1.717 | 0.615 | 0.000 | 1.379 |

These are observations on this machine, not production latency guarantees. Read docs/performance.md for instrumentation and experimental limitations.
