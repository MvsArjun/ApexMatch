# Recorded benchmark observations

Recorded: 2026-10-18T16:13:05.238014+00:00
CPU: AMD Ryzen 5 6600H with Radeon Graphics; OS: Windows-11-10.0.26300-SP0

Throughput is the median of repeated batch measurements. Latency columns are medians of per-repeat percentiles, not percentiles of pooled samples.

| Workload | Requests | Mode | requests/s | mean ns | p50 ns | p95 ns | p99 ns | allocations | peak requested MiB |
|---|---:|---|---:|---:|---:|---:|---:|---:|---:|
| noncrossing | 10,000 | baseline | 2,999,580 | 354 | 300 | 400 | 600 | 32,013 | 1.57 |
| noncrossing | 10,000 | pool | 3,541,327 | 313 | 200 | 300 | 500 | 75 | 6.08 |
| immediate | 10,000 | pool | 5,886,855 | 199 | 200 | 200 | 300 | 34 | 4.33 |
| immediate | 10,000 | baseline | 3,783,579 | 323 | 300 | 400 | 500 | 25,014 | 0.33 |
| cancellation_bursts | 10,000 | baseline | 2,511,238 | 347 | 300 | 500 | 900 | 20,196 | 0.21 |
| cancellation_bursts | 10,000 | pool | 4,845,666 | 227 | 200 | 300 | 400 | 47 | 2.24 |
| shared_level | 10,000 | pool | 5,253,480 | 205 | 100 | 200 | 200 | 60 | 5.83 |
| shared_level | 10,000 | baseline | 4,373,879 | 250 | 200 | 200 | 200 | 30,027 | 1.42 |
| large_book | 10,000 | baseline | 2,103,802 | 495 | 400 | 500 | 800 | 40,026 | 2.18 |
| large_book | 10,000 | pool | 2,530,941 | 423 | 300 | 400 | 900 | 82 | 7.06 |
| asymmetric | 10,000 | pool | 3,298,697 | 291 | 200 | 300 | 700 | 71 | 5.65 |
| asymmetric | 10,000 | baseline | 3,047,387 | 342 | 300 | 400 | 800 | 29,847 | 1.28 |
| noncrossing | 100,000 | baseline | 2,805,403 | 389 | 300 | 400 | 600 | 302,032 | 13.93 |
| noncrossing | 100,000 | pool | 4,151,858 | 263 | 200 | 300 | 400 | 100 | 26.88 |
| immediate | 100,000 | pool | 6,898,264 | 171 | 200 | 200 | 200 | 47 | 11.84 |
| immediate | 100,000 | baseline | 5,105,948 | 226 | 200 | 300 | 300 | 250,017 | 3.07 |
| cancellation_bursts | 100,000 | baseline | 4,147,863 | 263 | 200 | 300 | 500 | 197,247 | 1.58 |
| cancellation_bursts | 100,000 | pool | 5,664,278 | 203 | 200 | 300 | 300 | 60 | 10.03 |
| shared_level | 100,000 | pool | 6,682,661 | 181 | 100 | 200 | 200 | 85 | 26.63 |
| shared_level | 100,000 | baseline | 4,019,890 | 271 | 200 | 200 | 300 | 300,033 | 13.78 |
| large_book | 100,000 | baseline | 1,178,421 | 896 | 700 | 1,300 | 1,700 | 400,032 | 21.41 |
| large_book | 100,000 | pool | 1,472,843 | 761 | 600 | 1,200 | 1,600 | 118 | 40.97 |
| asymmetric | 100,000 | pool | 4,297,637 | 258 | 200 | 300 | 500 | 96 | 23.96 |
| asymmetric | 100,000 | baseline | 2,615,056 | 402 | 300 | 500 | 700 | 289,680 | 11.83 |
| noncrossing | 1,000,000 | baseline | 1,838,699 | 647 | 400 | 700 | 900 | 3,002,040 | 147.34 |
| noncrossing | 1,000,000 | pool | 3,061,688 | 366 | 300 | 400 | 600 | 129 | 199.41 |
| immediate | 1,000,000 | pool | 6,681,705 | 176 | 200 | 200 | 300 | 62 | 56.14 |
| immediate | 1,000,000 | baseline | 4,718,944 | 240 | 200 | 300 | 400 | 2,500,021 | 37.68 |
| cancellation_bursts | 1,000,000 | baseline | 4,047,254 | 285 | 300 | 400 | 600 | 1,969,455 | 18.86 |
| cancellation_bursts | 1,000,000 | pool | 5,437,288 | 209 | 200 | 300 | 400 | 75 | 32.57 |
| shared_level | 1,000,000 | pool | 5,997,591 | 185 | 100 | 200 | 200 | 114 | 199.16 |
| shared_level | 1,000,000 | baseline | 2,885,416 | 456 | 200 | 400 | 500 | 3,000,041 | 147.19 |
| large_book | 1,000,000 | baseline | 422,236 | 1,982 | 1,500 | 2,500 | 3,200 | 4,000,040 | 223.48 |
| large_book | 1,000,000 | pool | 539,203 | 1,864 | 1,600 | 2,500 | 3,200 | 157 | 332.75 |
| asymmetric | 1,000,000 | pool | 3,496,508 | 422 | 300 | 600 | 900 | 124 | 166.91 |
| asymmetric | 1,000,000 | baseline | 1,598,590 | 621 | 400 | 800 | 1,100 | 2,886,545 | 121.06 |

## Pool / baseline ratios

A throughput ratio above 1 favors the pool; a memory or p99 ratio above 1 means the pool used more memory or had a higher recorded tail.

| Workload | Requests | throughput ratio | p99 ratio | allocation-count ratio | peak memory ratio |
|---|---:|---:|---:|---:|---:|
| noncrossing | 10,000 | 1.181 | 0.833 | 0.002 | 3.877 |
| immediate | 10,000 | 1.556 | 0.600 | 0.001 | 13.226 |
| cancellation_bursts | 10,000 | 1.930 | 0.444 | 0.002 | 10.483 |
| shared_level | 10,000 | 1.201 | 1.000 | 0.002 | 4.114 |
| large_book | 10,000 | 1.203 | 1.125 | 0.002 | 3.237 |
| asymmetric | 10,000 | 1.082 | 0.875 | 0.002 | 4.400 |
| noncrossing | 100,000 | 1.480 | 0.667 | 0.000 | 1.930 |
| immediate | 100,000 | 1.351 | 0.667 | 0.000 | 3.852 |
| cancellation_bursts | 100,000 | 1.366 | 0.600 | 0.000 | 6.342 |
| shared_level | 100,000 | 1.662 | 0.667 | 0.000 | 1.933 |
| large_book | 100,000 | 1.250 | 0.941 | 0.000 | 1.914 |
| asymmetric | 100,000 | 1.643 | 0.714 | 0.000 | 2.024 |
| noncrossing | 1,000,000 | 1.665 | 0.667 | 0.000 | 1.353 |
| immediate | 1,000,000 | 1.416 | 0.750 | 0.000 | 1.490 |
| cancellation_bursts | 1,000,000 | 1.343 | 0.667 | 0.000 | 1.727 |
| shared_level | 1,000,000 | 2.079 | 0.400 | 0.000 | 1.353 |
| large_book | 1,000,000 | 1.277 | 1.000 | 0.000 | 1.489 |
| asymmetric | 1,000,000 | 2.187 | 0.818 | 0.000 | 1.379 |

These are observations on this machine, not production latency guarantees. Read docs/performance.md for instrumentation and experimental limitations.
