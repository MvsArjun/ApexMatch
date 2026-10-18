# Twenty implementation-based interview questions

Use these answers as explanations to understand and adapt, rather than scripts
to recite. Verification claims belong to [verification.md](verification.md);
measured performance and its limits belong to [performance.md](performance.md).

## 1. An older ask is priced at 102 and a newer ask at 101. Which fills first?

The ask at 101 fills first for an eligible incoming buy because price precedes
arrival time. The engine visits the lowest ask level, then the FIFO front of
that level. Arrival only decides between orders at the same price. The trade
uses the maker's price, even if the incoming buy's limit is higher. The sell
case is symmetric: visit the highest bid and require it to meet the sell limit.

Source: [`MatchingEngine::add`](../src/engine.cpp). Evidence:
`better_price_before_older_order` and `single_match_at_maker_price` in
[core_tests.cpp](../tests/core_tests.cpp).

## 2. Is matching O(1) because the best quote is immediately available?

No. Accessing a map endpoint is constant time, but resting a residual needs a
map lookup and hash/list allocation work. A sweep can touch K makers and remove
V levels. Each execution also emits an absolute book update through a helper
that looks up the affected level. Under average hash behavior the implemented
matching cost is O(K log L + V + log L), plus output allocation. Since V is at
most K, this is often summarized as O(K log L + log L). A pathological hash
collision pattern can make hash operations linear.

Cancellation is expected O(log L), rather than O(1): active-ID lookup is
average constant time, but locating the price map entry is logarithmic. A top-D
query walks at most D levels per side. Source:
[`OrderBook`, `MatchingEngine::book_event`, and `cancel`](../src/engine.cpp);
the complexity discussion in [design.md](design.md).

## 3. Why can the active-ID index safely retain list iterators across rehashes?

The retained iterator points into a separate FIFO list, not into the hash map.
Rehashing the index affects its own iterators and buckets but leaves list nodes
where they are. Map insertion also preserves existing levels. The index entry
is erased when its order node is removed; the owning level is erased when its
FIFO becomes empty. A redesign that used vector positions or pointers into a
relocating flat container would need a different stability argument.

Source: [`OrderBook::Location` and member containers](../include/apexmatch/engine.hpp),
[`MatchingEngine::add` and `cancel`](../src/engine.cpp). Evidence:
`cancellation_lifecycle_and_iterator_removal` and randomized invariants.

## 4. What prevents the pool from being destroyed before its containers?

Member declaration order. `pool_` is declared before the PMR maps and hash
containers, so reverse destruction destroys those containers first. Their
deallocation still reaches a live pool. An injected upstream resource remains
the caller's responsibility and must outlive the book. Copying is disabled;
the engine cannot accidentally copy an index containing iterators into another
book's lists. The pool is unsynchronized because the entire engine has one
owner, including queries.

Source: [`OrderBook` members and constructor](../include/apexmatch/engine.hpp),
[`OrderBook::OrderBook`](../src/engine.cpp). Evidence:
`allocation_failure_destruction` in [core_tests.cpp](../tests/core_tests.cpp).

## 5. A maker with quantity 10 trades 4, then is cancelled. What must happen?

Its node retains quantity 6, original arrival sequence and FIFO position after
the fill. The execution reduces its level aggregate by 4. Cancellation removes
only the remaining 6, updates the aggregate again and removes an empty level if
necessary. Previously executed quantity is never cancelled. Global accounting
must satisfy `submitted = 2 * traded + cancelled + resting` because each trade
consumes quantity from two accepted orders.

The tests additionally maintain a ledger per accepted ID, requiring
`submitted = executed + cancelled + remaining` and no overexecution. Source:
[`add`, `cancel`, and `check_invariants`](../src/engine.cpp); evidence:
`fifo_partial_keeps_priority`, cancellation cases and `randomized_reference`.

## 6. Why is checking each quantity against INT64_MAX insufficient?

Many valid orders at one level can sum beyond that bound. Aggregates use
`uint64_t`, and the engine rejects an add if its positive quantity exceeds
`UINT64_MAX - submitted_quantity`. Because this is a lifetime cap, every active
aggregate and cumulative quantity statistic remains bounded by accepted input.
It permits a level total up to `UINT64_MAX` but never wraps. Cancelling does not
restore budget, making the reasoning conservative and simple.

The engine never multiplies price by quantity; notional or P&L extensions would
need separate checked arithmetic. Source: [`MatchingEngine::add`](../src/engine.cpp).
Evidence: `arithmetic_boundaries`, including two `INT64_MAX` quantities plus one
unit and a maximum-sized execution.

## 7. Can event numbering overflow during a large sweep?

The engine preflights before incrementing the request counter. With N active
orders, a request can execute against at most N makers, generating at most
two events per fill plus acknowledgement and a residual update. It reserves a
conservative `2 * N + 3` event slots and N execution slots, first checking that
computing that bound cannot overflow. Insufficient capacity throws before
mutation and can refuse a small request earlier than strictly necessary.

The boundary test calls the same internal predicate used by `begin_request`.
It verifies near-maximum accept/reject decisions, not an injected live engine
with huge counters. Source: [`sequence_capacity.hpp`](../src/sequence_capacity.hpp)
and [`MatchingEngine::begin_request`](../src/engine.cpp). Evidence:
`sequence_capacity_boundaries`.

## 8. Why do you keep both an active-ID map and an accepted-ID set?

They represent different lifetimes. The map supports finding and cancelling
currently resting orders. The set enforces a session policy that an accepted
ID remains retired even after fill or cancellation. A rejected ID never enters
that set and can be retried with corrected fields. Keeping only active IDs
would silently change duplicate semantics after an order completed.

The cost is persistent O(A) accepted-ID history, where A is accepted orders over
the session, even if the active book is empty. Source:
[`OrderBook::active_`, `seen_`, and `MatchingEngine::add`](../src/engine.cpp).
Evidence: `duplicates_and_rejected_id_reuse`.

## 9. Does an allocation exception leave the engine unchanged?

No. Ordinary business rejection preserves book state, but unexpected resource
failure has a different contract: the session must be discarded. An allocation
can fail after an ID or statistics change, after a maker fills, or while building
events. The implementation does not claim transactional rollback or safe
continuation after that exception. RAII must still release owned storage when
the engine is destroyed.

The injected-resource test verifies destruction at successive book-allocation
failure points and zero outstanding injected bytes afterwards. It does not
simulate every global allocation or establish a strong exception guarantee.
Source: [engine.cpp](../src/engine.cpp), [core_tests.cpp](../tests/core_tests.cpp);
contract: [design.md](design.md).

The tested native GCC 16.2 pool runtime currently fails this case with an access
violation. An engine-free reproducer isolated the dependency's failure to
propagate an upstream exception; native Clang/libc++ passed. Do not describe
the complete GCC suite as passing. See [toolchain-issue.md](toolchain-issue.md).

## 10. How can an unordered index coexist with deterministic replay?

Matching never uses hash iteration to choose a maker. The input command order
sets request sequence; ordered prices and FIFO lists decide executions. Order
enumeration explicitly sorts by arrival. Events have fixed request, event and
execution sequences, integer values and locale-independent JSON output, with
no wall clock in the matching decision. Queries consume no mutation sequence.

Byte-identical replay assumes the same ordered input. Different clients racing
over TCP do not create an inherently deterministic global input order. Source:
[`orders`, `add`, `emit`](../src/engine.cpp), [protocol.cpp](../src/protocol.cpp).
Evidence: allocator-equivalent replay and [replay_test.py](../tests/replay_test.py).

## 11. What makes the reference engine independent enough to be useful?

It stores active orders in an unsorted vector and scans all candidates for best
compatible price and earliest arrival. Cancellation is a linear search and
depth is rebuilt from orders. It shares value types and intended policy, but
does not call the production validator or reuse map/list/index helpers. This
can catch structural and FIFO bugs that two copies of one algorithm would miss.

Eight fixed seed/allocator combinations compare 40,000 requests. Comparisons
include exact executions, event order, rejection reasons, depth, active order
contents, statistics and an independent per-order ledger. That remains bounded
testing, not a proof. Source: [reference_model.hpp](../tests/reference_model.hpp)
and `randomized_reference` in [core_tests.cpp](../tests/core_tests.cpp).

## 12. What concrete defect did the final audit find in the core API?

`event_json` indexed its event-name array using a caller-supplied enum value
without checking its range. `static_cast<EventKind>(99)` reproduced an
out-of-bounds UBSan diagnostic. The serializer now validates before indexing.
Other typed entrypoints also silently mapped unknown enums to sell, baseline
allocation, or a no-op command; they now throw `invalid_argument`.

The text parser could not generate an invalid event enum. The correction makes
the library API robust while preserving `add`'s normal invalid-side rejection.
Source: [`event_json` and `apply_command`](../src/protocol.cpp),
[`side_name`, `quantity_at`, constructor](../src/engine.cpp). Evidence:
`typed_enum_validation` and [core-audit.md](core-audit.md).

## 13. Why does the bounded queue use predicates with condition-variable waits?

The predicate is checked while holding the mutex and rechecked after waking,
so spurious wakes cannot push into a full queue or pop from an empty one. The
same mutex protects closed state and the deque, avoiding a lost wakeup between
checking state and sleeping. Push waits for space or closure; pop waits for data
or closure. Closing wakes both groups, prevents future pushes and lets consumers
drain existing FIFO items before receiving an empty optional.

Mutex unlock/lock provides visibility; no hand-written atomic memory order is
needed. Source: [`BoundedQueue::push`, `pop`, `close`](../include/apexmatch/extensions.hpp).
Evidence: queue tests in [extensions_tests.cpp](../tests/extensions_tests.cpp).

## 14. What deadlock could occur during pipeline initialization or failure?

A producer may fill the queue and block waiting for the consumer. If consumer
construction throws after that producer starts, a joining thread destructor
can wait forever because no consumer will drain the queue. `replay_pipeline`
constructs the engine before launching the producer. During output failure it
closes the queue before joining, waking a blocked push. After normal draining,
joining establishes visibility of the producer's stored exception before it
is rethrown.

Closing cannot interrupt a generic input stream blocked inside an OS read.
Finite or responsive input remains a stated requirement. Source:
[`replay_pipeline`](../src/extensions.cpp). Evidence: pipeline failure/drain
cases in [extensions_tests.cpp](../tests/extensions_tests.cpp).

## 15. How do partial reads and writes change the TCP implementation?

TCP preserves bytes, not application messages. The server accumulates bytes
until LF, handles several records in one receive, and retains an incomplete
record across receives. Each connection's send offset advances by the actual
write count; an unsent suffix remains queued for later writability. Would-block
and interrupted operations are handled without inventing sent bytes.

Each command ends with `RESPONSE_END`, so a client can distinguish a sweep
producing many execution records from an ignored command producing none.
Market-data subscriptions can also deliver unsolicited messages. Source:
[`Connection`, `run_local_server`, `run_local_client`](../src/extensions.cpp).
Evidence: fragmented and large-response cases in [network_test.py](../tests/network_test.py).

## 16. If a TCP acknowledgement disappears, may the client safely retry?

The missing reply does not establish whether the command executed. The server
may mutate its engine before the connection drops or a slow subscriber exceeds
its output bound. IDs prevent accepting the same ID twice within the current
session, but there is no persistent recovery, reconnect reconciliation or
exactly-once protocol. A production client would need explicit session and
recovery rules rather than assuming transport success equals business success.

Buffers are bounded and slow peers can be disconnected. Those limits do not
bound book size, accepted-ID history or the O(K) work/events of a large sweep.
Source: [`queue_output` and server processing](../src/extensions.cpp);
contract and limits: [extensions.md](extensions.md).

## 17. Why publish absolute market-data quantities instead of deltas?

`MD_LEVEL` replaces a side/price aggregate; zero removes it. A consumer need not
infer book quantity by subtracting executions or combining order-entry
acknowledgements. Its independent sequence increments once per level update.
After a changed request, `MD_TOP` and `MD_SNAPSHOT` describe final state at the
latest sequence; those checkpoints do not represent additional level changes.

A missing sequence requires recovery. A top-N snapshot only reconstructs its
advertised depth, not arbitrary deeper liquidity. Source:
[`MarketDataPublisher::publish_to` and `snapshot`](../src/extensions.cpp).
Evidence: publisher reconstruction tests and [network_test.py](../tests/network_test.py).

## 18. How does the TWAP scheduler avoid overflow and rounding loss?

Quantity is divided into `base = quantity / slices` and a remainder distributed
one unit at a time to the earliest children. No unit is lost and each child is
positive. Times use quotient/remainder decomposition of duration over intervals,
avoiding direct multiplication of a near-`UINT64_MAX` duration by an index.
The configured slice cap bounds the smaller remainder product. Child ID range
and time ordering are validated before simulation.

External events are stably ordered and precede children at equal times.
Surviving accepted children are cancelled after end-time work. Completion means
schedule and cleanup completed, not full execution. Source:
[`twap_schedule` and `simulate_twap`](../src/extensions.cpp). Evidence: `twap_tests`
in [extensions_tests.cpp](../tests/extensions_tests.cpp).

## 19. Why separate throughput, latency and allocation benchmark passes?

Batch throughput places clocks around the whole request loop. Per-request
latency timing adds clock calls around each request and perturbs the work, so
its percentile should not be inverted to claim batch throughput. Allocation
counting adds a wrapper resource and runs separately from both timing passes.
The passes use the same generated request vector and verify outcome agreement.
Parsing, JSON, I/O, construction, destruction and invariants are outside the
matching timing; event creation and result destruction are included.

Report repeat spread, timer resolution, build flags, workload, allocator and
machine metadata. A median of repeated p99 values is not a pooled-sample p99.
Source: [`batch`, `latencies`, `memory`](../benchmarks/benchmark.cpp) and
[run_benchmarks.py](../tools/run_benchmarks.py); interpretation: [performance.md](performance.md).

## 20. What would justify another performance optimization?

Start from a measured bottleneck on a specified workload. The pool experiment
changes only allocation policy, so it gives a defensible baseline comparison;
recorded requested-byte retention also shows its memory cost. Fewer upstream
allocations do not, by themselves, prove fewer cache misses, faster hardware
paths or universally better tails. Laptop timing differences need repeatability
and an honest account of scheduler and clock noise.

Use available sampled profiles and counters to distinguish allocation, tree
walks, event-vector growth and other costs. Preserve the old result, change one
thing, rerun correctness and compare throughput, tails and memory. Hardware
counter or Linux profiling claims require actual runs. Source:
[benchmark.cpp](../benchmarks/benchmark.cpp), [profile.sh](../tools/profile.sh),
[performance.md](performance.md) and [verification.md](verification.md).
