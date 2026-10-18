# Study the completed ApexMatch code

Start with a release/debug build and the tests described in the [README](../README.md). This guide assumes comfort with C++ syntax, maps, lists, and basic threads. Read one working component at a time, trace the example, then read its tests. All required code is supplied; optional modifications at the end are for practice after understanding the delivered behavior.

## 1. Learn the domain using five requests

**Problem:** match compatible buyers and sellers fairly while retaining the rest. A limit buy says “pay at most this price”; a limit sell says “receive at least this price.” A market order requests immediate execution without a price bound; ApexMatch implements limit orders only. A bid is a resting buy, an ask a resting sell. The highest bid and lowest ask are the best prices; ask minus bid is the spread. Market depth groups remaining quantity by price. The engine enforces matching rules; a strategy decides when and what to submit.

Read the value types and public methods in [engine.hpp](../include/apexmatch/engine.hpp), then run these through `apex_cli --check`:

```text
ADD 1 SELL 101 5
ADD 2 SELL 101 7
ADD 3 SELL 102 4
ADD 4 BUY 102 10
CANCEL 2
BOOK
STATS
```

Sell 1 executes first for 5 at 101, then sell 2 for 5 at 101. Sell 2 keeps its original place with 2 remaining. Sell 3 remains untouched at 102. Cancelling 2 removes only its 2 remaining. The final ask is 4 at 102. The resting sellers are makers; incoming buy 4 is the taker. Trading at a price better than the taker's limit is normal: execution uses the maker price.

**C++ ideas:** small value types, enum classes, signed price/quantity types, unsigned identifiers, `std::optional` for an empty best quote. The order's arrival sequence orders requests deterministically; an execution sequence identifies each trade. An order ID identifies one accepted order across its lifetime.

**Tests:** the first matching/FIFO/multi-level/cancellation cases in [core_tests.cpp](../tests/core_tests.cpp). **Interview discussion:** why “best price first” can override age across different prices, why a partial maker retains age, and why submitted buy/sell quantity counts both sides of an execution.

## 2. Understand ownership before the algorithm

**Problem:** answer price queries, preserve FIFO, and cancel arbitrary IDs without searching every resting order. Read `OrderBook`, `Level`, and `Location` in [engine.hpp](../include/apexmatch/engine.hpp), then its query methods in [engine.cpp](../src/engine.cpp).

- Each side is a `std::pmr::map<Price, Level>` in ascending price order. The best ask is `begin()`; the best bid is `rbegin()`.
- Each level owns a `std::pmr::list<Order>`. New resting orders append; matches consume the front.
- `active_` maps an ID to a side, price, and list iterator. It identifies the exact order node for cancellation.
- `seen_` retains all accepted IDs, even after fill/cancel, implementing session-long non-reuse.
- `MatchingEngine` owns the `OrderBook` and all mutations. Queries return copies or a const book reference, so callers cannot mutate engine state through the public API.

**Iterator reasoning:** inserting in a list preserves existing iterators; erasing invalidates only the erased node. Rehashing `active_` may invalidate its own hash iterators but does not invalidate the list iterators stored as values. Map insertion preserves other map iterators. Removing a whole level is safe only after removing every order it owned and its active index entries. Cancellation stores a copy of the location before erasing the index entry.

**Lifetime reasoning:** the optional pool is declared before containers, so reverse member destruction destroys the containers before the pool. Books cannot be copied; copying internal iterators would point into the old book. Resource ownership is explicit and follows RAII.

**Example:** three buys at 100 form list `[1,2,3]`. Cancelling 2 erases its node directly; 1 and 3 retain order. Cancelling the last order at a level removes the level node as well. **Tests:** `cancellation_lifecycle_and_iterator_removal`, depth-order, empty-book, and final-level-removal coverage in [core_tests.cpp](../tests/core_tests.cpp).

**Complexity/failures:** price lookup is logarithmic in price levels; hash ID lookup is expected constant time, with linear worst cases. A list node costs allocation and pointer chasing. History storage is proportional to all historical accepted IDs, not just the active book. Discuss these costs before proposing a faster container.

## 3. Follow one mutation end to end

**Problem:** apply a request atomically with respect to other requests and emit a reproducible explanation of its effects. Read `MatchingEngine::add`, `cancel`, `begin_request`, `emit`, and `book_event` in [engine.cpp](../src/engine.cpp).

For add, validation and duplicate/lifetime quantity checks run first. An accepted ID enters history, counters change, and an accepted acknowledgement is emitted. Repeatedly inspect the best opposite price: stop if incompatible; otherwise execute `min(taker_remaining, maker_remaining)`. Decrement quantities, remove a zero maker, remove an empty level, emit an execution and its affected-level update. Finally append any residual incoming quantity to its own level and emit that level update.

Trace a deeper sweep: sells 1 `2@100`, 2 `3@101`, 3 `4@101`; incoming buy 4 `8@102`. Executions are 2 at 100, 3 at 101, 3 at 101. Sell 3 remains 1 at 101; buy 4 does not rest. There are three executions even though only two levels were visited.

**Event ordering:** accepted first; each execution followed by the affected resting-side level update; any taker residual update last. Cancellation emits cancelled then the affected level update. Rejection emits only rejected. Each state-changing/malformed request consumes one request sequence; each emitted event has a new global event sequence. A query/comment consumes neither. Book-update quantity is an absolute level quantity, and zero means deletion.

**C++ ideas:** references avoid copying a maker, `std::min` prevents overfill, iterator erasure prevents dangling access, moving event strings avoids unnecessary copies, a vector returns events by value with normal move/elision semantics. Avoid holding a reference to a maker after removing its list node.

**Complexity:** with `K` executions and `P` current levels, this implementation has an expected upper bound of `O((K+1) log(P+1))` plus emitted-event storage and hash costs. It performs an affected-level lookup for each execution. Even an ideal tree boundary lookup cannot eliminate the `K` executions. Cancellation is expected hash lookup plus `O(log P)` price lookup and constant-time list erasure. Hash worst cases and occasional rehash/allocation spikes remain possible.

**Tests:** exact event-order/sequence, maker-price, FIFO, multi-order sweep, partial cancellation, and deterministic allocator-equivalence cases in [core_tests.cpp](../tests/core_tests.cpp). **Failure cases:** inactive cancellation, invalid numeric fields, duplicates, lifetime counter overflow, allocation failure. Business rejections preserve resting state; the simulator does not promise transaction rollback after arbitrary allocation failure. Treat resource exhaustion as fatal and rebuild from a known input stream.

## 4. Read validation and the protocol boundary

**Problem:** distinguish malformed syntax from syntactically valid but invalid business values. Read `parse_command`, `apply_command`, and `process_line` in [protocol.cpp](../src/protocol.cpp), then [cli.cpp](../apps/cli.cpp).

`parse_command` tokenizes bounded lines and uses `std::from_chars`, checking both range errors and full-token consumption. `ADD 1 BUY 100x 4`, an overflowing integer, and extra arguments are rejected; lowercase command/side spellings do not silently normalize. Leading `+` is rejected. A negative signed price can parse successfully and then fail domain validation. An ID is unsigned and must be nonzero. Prices/quantities must be positive signed 64-bit values.

Comments start when the first token begins with `#`; blank/comment lines are ignored. `BOOK` and `STATS` query current state. JSON numbers are integers and formatting uses the classic locale. Avoid passing IDs through JavaScript `Number` if they exceed its exact integer range; preserve JSON integer text or use a suitable integer parser.

**Example:** `ADD 7 BUY 0 2` creates a rejected request but does not reserve ID 7. A later valid add may use 7. After 7 is accepted and cancelled, it remains permanently unavailable for that engine session. `CANCEL 7` then rejects as inactive.

**C++ ideas:** `string_view` avoids copying token contents but may not outlive the input line; `Command` owns its error string. `from_chars` avoids locale-dependent parsing. Overflow checks subtract from the maximum before adding. The engine avoids price-times-quantity notional arithmetic entirely; adding such arithmetic later requires its own checks.

**Tests:** strict-parser, invalid-state-preservation, arithmetic-boundary, rejected-ID reuse, JSON escaping, and query-sequencing cases in [core_tests.cpp](../tests/core_tests.cpp); golden replay in [replay_test.py](../tests/replay_test.py). **Interview discussion:** syntax and domain validation are different layers; rejected requests still belong in deterministic sequence history.

## 5. Establish correctness with independent evidence

**Problem:** a handful of happy paths cannot establish book consistency. Read `OrderBook::check_invariants` and `MatchingEngine::check_invariants`, then [reference_model.hpp](../tests/reference_model.hpp) and randomized comparisons in [core_tests.cpp](../tests/core_tests.cpp).

The checker walks each level, verifies positive quantities, strictly increasing arrival within FIFO, aggregate sums, active-index/list correspondence, accepted-ID membership, price order, and an uncrossed final book. Global quantity conservation is:

```text
submitted_quantity = 2 * traded_quantity + cancelled_quantity + resting_quantity
```

Both sides of each trade were submitted, hence the factor two. Tests also keep a per-order ledger: executed plus cancelled plus remaining equals the original submitted quantity, and executed never exceeds submitted. This catches errors that could cancel out in global totals.

The reference model deliberately uses a simpler representation and selection algorithm, choosing eligible makers by price/arrival instead of reproducing the production container topology. Fixed-seed add/cancel/rejection streams compare outcomes, trades, and full book state for both allocation modes after every step. A seed plus step identifies a reproducible failure.

**Example:** incorrectly advancing a partially filled maker behind its neighbor can preserve aggregates and conservation while violating FIFO. A FIFO test and reference execution comparison catch it. An orphan active index might preserve displayed depth but fail the one-to-one invariant.

**Complexity/failures:** invariant checks are diagnostic full scans with temporary storage; they are deliberately outside core benchmark timing. Randomized tests explore many cases but do not prove all possible inputs. ASan/UBSan look for classes of memory/undefined behavior errors; TSan observes exercised data races. See the recorded verification status before claiming a check ran.

**Interview discussion:** explain why independent algorithms matter more than a second copy of the same code; distinguish business correctness, structural invariants, sanitizers, and end-to-end tests.

## 6. Move commands between threads without sharing the book

**Problem:** let input production overlap matching while bounding queued work. Read `BoundedQueue<T>` in [extensions.hpp](../include/apexmatch/extensions.hpp), `replay_pipeline` in [extensions.cpp](../src/extensions.cpp), and [pipeline.cpp](../apps/pipeline.cpp).

A producer reads strings and moves them into a bounded deque. The consumer pops in FIFO order and alone owns the matching engine. `push` waits while full; `pop` waits while empty. A mutex protects both the deque and `closed_`. Predicate waits handle spurious wakeups. Closing prevents further pushes, wakes both sides, and allows the consumer to drain already queued items before `pop` returns empty.

**Example:** capacity 1, producer commands A/B/C. After pushing A, pushing B waits until the consumer removes A. Closure after C does not discard C. With a consumer/output error, closing wakes a producer blocked on a full queue before joining it.

**C++ ideas:** `unique_lock` permits condition-variable waits to release/reacquire the mutex; `lock_guard` handles simple critical sections; `jthread` supplies lifetime joining; an `exception_ptr` transfers a producer exception after `join` establishes synchronization. `std::move` transfers strings through the queue. Acquire/release visibility comes from mutex lock/unlock, not manually selected atomic memory orders.

**Complexity/failures:** deque push/pop are constant time in container terms, but may allocate or block without a bounded wait time. Capacity bounds item count, not arbitrary string bytes. Input streams must be finite/responsive for prompt shutdown; closing the queue cannot forcibly interrupt an external blocking read. Slow output propagates backpressure through the consumer to the producer. The core never has two concurrent owners.

**Tests:** queue ordering/capacity/closure/no-loss and replay equivalence in [extensions_tests.cpp](../tests/extensions_tests.cpp). **Interview discussion:** mutex correctness before lock-free complexity; why no SPSC atomics/false-sharing padding is needed for this mutex design; an SPSC ring would need separate publication/consumption synchronization and measurement.

## 7. Reconstruct published market data

**Problem:** clients need displayed liquidity without interpreting order acknowledgements as depth. Read `MarketDataPublisher` in [extensions.hpp](../include/apexmatch/extensions.hpp) and [extensions.cpp](../src/extensions.cpp).

`MD_LEVEL` copies an absolute side/price quantity and order count from engine book-update events. Zero deletes that level. The publisher increments a separate market-data sequence for each level update. After a changed request it emits `MD_TOP` and a depth-limited `MD_SNAPSHOT` at the latest market-data sequence. These final-state messages do not consume additional sequence values. A snapshot request uses the current sequence and does not mutate it.

**Example:** two executions at ask 101 can publish level quantities 7 then 2 in one request, followed by final best prices and depth. Apply level messages in order. On a sequence gap, request a snapshot rather than pretending a missing update did not matter. A top-N snapshot describes only the configured depth; it is not the full book.

**C++ ideas:** const book queries separate observation from mutation; `optional` represents an absent side. **Tests:** publisher state/snapshot assertions in [extensions_tests.cpp](../tests/extensions_tests.cpp) and subscriptions in [network_test.py](../tests/network_test.py). **Complexity/failures:** publishing costs at least proportional to changed levels plus returned snapshot depth and JSON bytes; a depth snapshot allocates. This work is excluded from core matching benchmarks. Market-data sequence overflow fails explicitly.

## 8. Trace a command over local TCP

**Problem:** reuse the protocol under real stream framing and partial I/O. Read `run_local_server`/`run_local_client` in [extensions.cpp](../src/extensions.cpp), then [server.cpp](../apps/server.cpp), [client.cpp](../apps/client.cpp), and [network_test.py](../tests/network_test.py).

The server binds only `127.0.0.1`, uses nonblocking sockets and `select`, and owns the engine in one event-loop thread. Each connection retains an incomplete input line, pending output, and the offset already sent. TCP is a byte stream: one read may contain half a command, several commands, or a command plus part of the next. Newline frames commands. The sender advances its output offset after partial writes instead of assuming one send writes everything.

Each command response ends with `RESPONSE_END` carrying a connection-local command sequence. `SUBSCRIBE` requests a snapshot and subsequent market-data publications; `UNSUBSCRIBE` stops them. `SHUTDOWN` requests local simulator shutdown. Responses to accepted work are drained subject to the server's bounded shutdown behavior. An incomplete line at EOF is a protocol error; it is not silently treated as a complete command.

**Example:** send `ADD 1 BU`, then `Y 100 3\nBOOK\n`. No order is processed before the first newline. The completed ADD is processed before BOOK on that connection. Across multiple connections, arrival/readiness order determines engine order; the server is deterministic for its serialized command stream, not for uncontrolled OS/network interleavings.

**C++ ideas:** RAII socket handles close on every path; platform wrappers isolate POSIX/Winsock differences; buffer offsets encode partial progress. **Complexity/failures:** readiness scanning costs scale with connection count; line/output/client bounds prevent unbounded retained buffers. Slow readers cause read throttling and eventual disconnect when output limits are exceeded. There is no authentication, persistence, reconnect recovery, or fairness guarantee. UDP could drop, duplicate, or reorder datagrams; it would need sequence/gap handling and recovery, not just another socket type.

**Tests:** partial framing, bad input, response ordering, subscriptions, closure, limits, and shutdown in [network_test.py](../tests/network_test.py). **Interview discussion:** TCP delivery order is not message framing or exactly-once application semantics; an acknowledgement can be lost with a connection after the engine accepted an order.

## 9. Simulate a TWAP client without assuming fills

**Problem:** split a parent quantity over a deterministic interval, while using the same matching rules as every other source. Read `twap_schedule`, `simulate_twap`, and `read_timed_commands` in [extensions.cpp](../src/extensions.cpp), then [twap.cpp](../apps/twap.cpp).

Quantity 23 across 5 slices becomes `[5,5,5,4,4]`. From time 0 to 100, children are scheduled at `[0,25,50,75,100]`; a single slice occurs at the start. Integer division rounds time offsets down. The first remainder children receive one extra unit. IDs are consecutive from `first_child_id`, all use the configured side and fixed limit price. No wall-clock sleep drives the simulation.

External commands are stably sorted by simulated time, preserving input order at equal times. All external events at a child's timestamp run before that child. At the end, any surviving children are cancelled. Submitted, accepted, rejected, executed, unfilled, and cancelled quantities are reported separately.

**Example:** the default app supplies 8 sell units at 100, then 5 at 101, then liquidity at 102. A buy limit of 101 can execute only the first 13 units. Submitting all 23 therefore leaves 10 unfilled, which are cancelled at completion. A successful schedule does not imply a completed execution target. Rejected children count toward submitted/rejected but not accepted/unfilled.

**C++ ideas:** unsigned deterministic time, overflow-safe duration decomposition, validated ID ranges, stable sorting, vectors of value types. **Tests:** rounding/schedule boundaries, equal-time ordering, unfilled/rejected children, conservation, and deterministic reports in [extensions_tests.cpp](../tests/extensions_tests.cpp). **Complexity/failures:** schedule construction is `O(slices)`; sorting external events is `O(M log M)`; matching cost is additional. Invalid time ranges, zero/excess slices, ID overflow, and out-of-interval market events fail before simulation. This client uses the shared in-process engine interface, while the separate TCP client demonstrates transport.

## 10. Read measurements after correctness

**Problem:** decide whether a change improves the workloads you actually measured. Read [performance.md](performance.md), [benchmark.cpp](../benchmarks/benchmark.cpp), and [run_benchmarks.py](../tools/run_benchmarks.py), then the recorded [results](../results/benchmark/summary.md).

The allocation experiment changes only the book resource: ordinary allocation versus an unsynchronized pool. Both modes preserve identical containers and semantics. Trace the three independent passes: batched throughput, individual-request latency, and allocation scan. Understand why event-vector destruction is timed but excluded from book-resource allocation counts, output is excluded, per-request clocks perturb latency, and requested bytes differ from RSS. The counting upstream resource exists only in the untimed allocation pass.

**Example:** cancellation bursts can reuse pool nodes, while a growing distinct-level book retains many nodes and may pay extra chunk capacity. If p99 rises despite higher throughput, both observations matter. A faster average does not prove a better worst case. Do not infer cache behavior from timing alone; inspect profiler evidence when available.

**Tests:** both allocator modes run fixed-seed reference comparisons. Benchmark passes also check structural invariants and outcome counts. **C++ ideas:** PMR routes allocations at runtime without changing matching containers, a custom resource forwards sizes/alignments, resource lifetime, monotonic clocks, and statistical summaries. **Interview discussion:** formulate a hypothesis, change one cause, preserve the baseline, measure repeat spread, and state platform/instrumentation limits.

## Optional modifications after reviewing the finished code

1. Add an order-replacement command with a precise policy for quantity reduction versus price/quantity increases. Decide when time priority changes and add independent-model tests first.
2. Reserve hash capacity from an explicit workload estimate; measure rehash/tail tradeoffs without simultaneously changing the allocator.
3. Add an event-sink interface to compare event-vector allocation with reusable/batched publication; keep deterministic event order unchanged.
4. Implement an SPSC ring as a separate queue variant, document acquire/release publication and wraparound, then verify loss/order under TSan and compare against the mutex queue.
5. Add snapshot plus ordered-input checkpoint recovery, with a test showing a restarted engine produces the same future events and ID-reuse policy.
6. Add a networked TWAP adapter using the existing schedule and TCP response framing; handle accepted-versus-filled state explicitly rather than inferring fills from successful writes.

These exercises extend an implemented system; none is required to use or explain the delivered simulator.
