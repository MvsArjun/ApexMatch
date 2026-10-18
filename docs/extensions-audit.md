# Extension engineering audit

Review date: 2026-10-18. Scope: `include/apexmatch/extensions.hpp`, `src/extensions.cpp`, the pipeline/server/client/TWAP applications, and their C++ and Python tests. This reviews the existing implementation; no replacement architecture or external dependency was introduced.

## Classification and evidence

| Component | Classification after fixes | Evidence |
| --- | --- | --- |
| Bounded queue | Verified in native Windows and hosted Linux libc++ suites | Mutex-protected state and predicate waits; close/drain, blocked waiter wakeup, 50,000-value FIFO, and concurrent two-producer/two-consumer checks; full Linux TSan run passed. |
| Replay pipeline | Verified in native Windows and hosted Linux libc++ suites | Byte-for-byte sequential replay comparison at capacities 1, 2, and 64; oversized-line recovery; finite EOF; injected input failure drains prior commands and propagates; output failure closes and joins. |
| Market data | Verified in native Windows and hosted Linux libc++ suites | Absolute level replacements reconstruct emitted depth; independent sequence, top-of-book, final snapshots, rejection silence; actual JSON reconstruction in TCP integration. |
| Localhost TCP server | Verified in native Windows and hosted Linux suites | Fragmented and batched requests, half-close, malformed/oversized input, bounded-output disconnect, healthy-peer continuation, FIFO sweep spanning multiple send/receive calls, explicit shutdown; Linux POSIX SIGTERM draining. |
| Blocking TCP client | Defects corrected; verified in native Windows and hosted Linux suites | Exact and over-limit response frames from a controlled peer; command limit before EOF; finite-EOF exact-limit command; normal server integration and multi-read responses. |
| Simulated TWAP | Verified in native Windows and hosted Linux libc++ suites | Quantity/time rounding, stable tie ordering, zero-duration schedules, delayed fills, collisions and external cancellation, reproducibility, maximal quantity/ID/time cancellation accounting. |
| Linux ASan/UBSan, leak detection and TSan | Full Clang 21/libc++ jobs passed | Six CTest groups per job; leak detection enabled in ASan and separate TSan configuration; no sanitizer diagnostics in passing logs. The original libstdc++ C++ jobs remain failed on the mandatory core resource-failure case. |

## Defects corrected

The client checked its buffered response length only after looking for a newline. If an oversized response and its delimiter arrived together, it returned the complete line before applying the 1 MiB bound. The added `client_bounds_test` reproduced this against the original Release executable: it failed with `client accepted a framed response over 1 MiB`. The receiver now validates complete frame length and limits each read to the remaining allowance plus one byte. Both exactly 1 MiB and 1 MiB + 1 are covered by a real socket test.

Client command input used `getline` and checked the 4096-byte limit afterward. A long command therefore retained arbitrary input and could wait indefinitely for its delimiter. The reader now rejects on the 4097th byte. The regression leaves stdin open after that byte and requires the process to exit with an error before EOF. A separate boundary case preserves a final 4096-byte command at finite EOF.

The pipeline now constructs its consumer engine before starting the producer. This is exception-path hardening: if later initialization could throw while a producer was blocked on a full queue, destruction of its joining thread could wait forever. No injected engine-constructor failure is claimed; moving initialization removes the problematic ordering without changing successful replay behavior.

The client now checks socket timeout configuration and detects failure writing its initial greeting response. The previous code ignored the timeout setup result and could return success with a failed output stream when there were no commands. These small error-path fixes follow direct source review; no OS-induced `setsockopt` failure is claimed.

## Concurrency and ownership conclusions

The matching engine is touched only by the replay consumer or the server's event-loop thread. Queue fields are always accessed under the same mutex; condition predicates make closure and availability visible without separate atomic ordering. `producer_error` is read only after joining the producer, establishing synchronization. The added two-consumer test uses a distinct result vector per consumer and checks duplicates only after joining.

The server owns socket handles through RAII. Connection buffers, subscription flags, command counters and engine state are confined to one thread. `select` writability drives retained unsent suffixes, and reads pause at half the configured output allowance. Overflow disconnects the affected peer. Shutdown marks all peers closing and drains pending output subject to a two-second deadline.

The TWAP scheduler decomposes duration multiplication into quotient/remainder terms; the validated slice cap bounds the remainder product. Scheduled quantities sum to the positive signed parent quantity, so accepted/executed/cancelled strategy totals remain within that bound. Only successfully accepted child IDs enter strategy accounting or terminal cancellation; other market orders and rejected-ID collisions are preserved.

## Verification performed for this audit

A separate native Windows Debug build using Clang 23.1.3 and Ninja passed all four CTest groups, including 29 named C++ cases at that point, in 19.08 seconds. This includes the strengthened concurrent queue, pipeline input-failure, TWAP boundary, and client response/input limit tests. The final verification report records the final source revision and wider configuration matrix; these timings are observations, not performance claims.

Hosted Ubuntu 24.04 [run 39149291801](https://github.com/MvsArjun/ApexMatch/actions/runs/39149291801), commit `8337a3d91200e990ab0586859871606ffe8136e4`, subsequently passed all six CTest groups in four Clang 21.1.8/libc++ 21.1.8 jobs: Debug, Release, ASan/UBSan with leak detection enabled, and separate TSan. No tests were skipped. This exercises the C++ queue/pipeline/market-data/TWAP cases, the actual network programs, POSIX SIGTERM draining, replay and tooling. The standalone pool probes also propagated `bad_alloc` safely; those small probes are uninstrumented, while the full application tests use the selected sanitizer configuration.

All seven original Linux jobs using runner-default libstdc++ remain failed in the mandatory core allocation-failure case, with five other CTest groups passing per job. GCC compiler 13.3.0 and Clang compiler 18.1.3 were observed; the shared libstdc++ package version was not recorded. The libc++ results verify this extension coverage but do not make the overall workflow green. See [hosted evidence](../results/hosted-ci/39149291801/README.md), [toolchain issue](toolchain-issue.md) and the final [verification report](verification.md).

Reproduce the relevant checks after configuring a build with its runtime on PATH:

```sh
cmake --build build/debug --parallel
ctest --test-dir build/debug --output-on-failure
python3 tests/network_test.py --server build/debug/apex_server --client build/debug/apex_client
```

On Windows use the `.exe` paths and the Python command available in the environment. The Linux, TSan and leak-enabled ASan results above come from actual hosted logs. These reproduction commands alone do not establish another host's result; [testing.md](testing.md) describes the explicit Clang 21/libc++ configuration used for the passing Linux matrix.

## Retained limitations

- Closing the queue cannot interrupt a generic input stream blocked in the OS. Finite-file replay is the unattended workflow; interactive stdin must finish or close. Queues must outlive every user and must not be destroyed while operations are active.
- `BoundedQueue<T>` moves values under its mutex. Applications should use nonthrowing moves, as the string replay and integer stress workloads do; arbitrary throwing move types do not receive a transactional delivery guarantee.
- The server uses `select`, a fixed client cap and one owner; it is a local simulator rather than a high-connection-count exchange gateway. Book size, used-ID history and per-command match-result storage are not capped by the network output allowance.
- Orders may execute before a peer loses its acknowledgement. Disconnect does not cancel its orders, and the protocol provides no durable recovery, idempotent retries, authentication or encryption.
- Market-data increments cover all changed levels, but the initial snapshot contains only top-N depth. It does not initialize unknown deeper levels. Publication sink failure has no rollback protocol; the supplied server exits on propagated exceptions.
- The client accepts the supplied server's response schema and uses the `RESPONSE_END` marker to finish a command. It does not implement independent JSON/schema validation, recovery or an end-to-end deadline.
- TWAP retains its input market vector and event timeline in memory and applies no global file-size/event-count cap. It is deterministic simulated time with limit orders, not a timed broker-connected execution service or a fill guarantee.
