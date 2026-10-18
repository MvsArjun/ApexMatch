# Interview questions with worked answers

Use these answers as explanations to understand, not scripts to memorize. Connect every claim to the delivered [core](../src/engine.cpp), [extensions](../src/extensions.cpp), [tests](../tests/core_tests.cpp), and [performance methodology](performance.md).

## 1. What exactly does price-time priority mean?

For a buy taker, choose the lowest compatible ask; for a sell taker, choose the highest compatible bid. Only within that best price do you choose the earliest arrival. An older ask at 102 loses to a newer ask at 101 when a buy limit is 102. The ordered maps supply price order, while each level's list supplies time order. The engine assigns arrival from its monotonically increasing request sequence rather than a wall-clock timestamp, avoiding clock ties and nondeterminism.

Trades execute at the resting maker's price. A buy limit of 105 that consumes an ask at 101 trades at 101. The limit is a bound, not the execution-price instruction. Once no compatible maker remains, unmatched taker quantity becomes a resting limit order at its own price.

## 2. How do partial fills preserve fairness?

Let a maker have remaining 7 and a taker need 3. Execute `min(7,3)=3`, leaving maker 4 in exactly the same list node. It is still ahead of later makers at that price. Removing and appending the maker would incorrectly change its time priority. Conversely, a fully filled maker must be removed from both the FIFO and active-ID index; an empty level must also disappear.

Consider makers `[A:5, B:7]` at 100 and a buy of 8. A fills 5, B fills 3, B remains 4 at the front. A later buy first sees B. Tests check both trade order and resulting quantities; aggregate quantity alone would not detect every priority bug.

## 3. Why maintain a separate active-order index?

Without it, cancellation requires searching levels and orders, potentially `O(A)` work for `A` active orders. `active_` maps an ID to side, price, and the exact list iterator. Expected ID lookup is constant time; locating the level by stored price costs `O(log P)` for `P` levels; list erasure is constant time. This implementation's cancellation is therefore expected `O(log P)`, not simply `O(1)`.

The index is redundant state, which creates an invariant: every live order appears exactly once in the index, and every indexed location names that same live node. The invariant checker verifies that correspondence. Redundant lookup structures buy speed in exchange for mutation bookkeeping and memory.

## 4. Why maps and lists instead of vectors?

Maps provide ordered prices and logarithmic insertion/lookup without a fixed price range. Lists provide stable node iterators and arbitrary constant-time removal once a node is known. These are straightforward, teachable correctness choices, but tree/list nodes allocate separately and follow pointers, often losing cache locality.

A vector can pack orders densely but erasing an element may move others and invalidate stored positions. A deque has different invalidation rules; it cannot be substituted casually. A dense tick-indexed array may be excellent for a bounded price range but wastes space for sparse or huge ranges. Intrusive lists or arena-indexed nodes reduce allocation/indirection but make lifetime and reuse logic more complex. Explain the workload and invariants before selecting an alternative.

## 5. What invalidates the stored iterators?

The stored iterators refer to list nodes, not to hash-table entries. Inserting or rehashing `active_` does not move those list nodes. List insertion preserves existing iterators; erasing a node invalidates that node's iterator. Erasing an empty map level destroys its list, which is why it may occur only after all relevant orders/index entries have been removed.

Be careful with references in the match loop. Once `pop_front()` removes a fully filled maker, its reference is dangling. The implementation constructs the trade and reads its ID before erasure, then uses independent values afterwards. Copying the whole book would copy iterators pointing into the original, so the book is explicitly noncopyable.

## 6. What is matching complexity?

There is no correct bound that ignores executions. If one incoming order trades with `K` resting makers, the engine must perform and emit those `K` executions. `P` is the number of price levels, which may be much smaller than the number of orders. This implementation repeatedly selects a best map boundary and does a price lookup when constructing the affected-level event, giving a conservative expected bound `O((K+1) log(P+1))`, plus output storage and hash behavior.

If many makers share one level, there can still be many executions while visiting only one price. With an alternate event path the loop might approach `O(K + L log P)` for `L` deleted/inserted levels, but that is not the delivered implementation's per-execution lookup behavior. Hash operations are expected constant time; rehashes and collision worst cases prevent claiming deterministic constant time.

## 7. What happens when cancelling inactive IDs or reusing IDs?

Cancellation removes only the live remaining quantity and emits a cancelled event followed by a level update. It does not edit historical trades. Unknown, fully filled, or already cancelled IDs reject as `inactive_order`; ID zero rejects as invalid. The engine deliberately does not retain different terminal statuses for cancellation responses.

Every accepted add ID enters `seen_` permanently for that engine session, preventing reuse after execution/cancellation. An invalid/rejected add does not reserve the ID, so a later corrected request can use it. This choice makes replay and client deduplication easier to explain but makes history memory grow with lifetime accepted requests. It is not a bounded-memory production deduplication service.

## 8. How do you ensure deterministic replay?

Use integer prices/quantities, explicit input order, request/event/execution sequences, deterministic matching rules, and fixed JSON formatting. Wall-clock timestamps and random values never influence core decisions. The same serialized commands produce byte-identical event output. Unordered-map iteration is not used to determine execution priority; when a list of all orders is requested for testing, it is sorted by arrival.

Determinism has a boundary: simultaneous network connections can be ordered differently by OS readiness and scheduling. Once the server serializes them, the core is deterministic for that chosen stream. Recording that ordered stream is necessary to reproduce a concurrent network run exactly. Likewise, deterministic TWAP uses simulated integer time and a defined tie rule, not real sleep timing.

## 9. How are sequences different from IDs?

An order ID is supplied by the client and identifies a particular accepted order. Request sequence counts engine mutation/rejection requests in processing order, including malformed commands routed to rejection. Event sequence counts emitted events globally, so a single sweep can consume several event numbers. Execution sequence counts trades only.

Queries and ignored comments consume no core request/event sequence. Rejected requests consume a request and emit a rejection event, while leaving resting state unchanged. Distinguishing these counters lets consumers reconstruct exact event order without assuming every order has one execution or every request has one event.

## 10. Why integers, and where can overflow still happen?

Integer ticks avoid binary floating-point equality/rounding surprises in price ordering. They do not make arithmetic automatically safe. Individual price and quantity use positive signed 64-bit values; aggregate/lifetime counters use unsigned 64-bit. The parser rejects out-of-range tokens with `from_chars`, and the engine checks lifetime submitted quantity before accepting another add.

That lifetime bound also bounds active aggregate sums. Each execution is at most the positive maker/taker remainder; conservation bounds traded totals. Sequence capacity is checked before mutation using a conservative upper bound for emitted events. Notional `price * quantity` is not computed; adding P&L or notional accounting would need checked multiplication or a wider arithmetic design. A safe addition test uses `new_value <= max - current`, avoiding overflow while testing for overflow.

## 11. What does quantity conservation prove, and what does it miss?

Across both sides, `submitted = 2*traded + cancelled + resting`. Each trade removes one unit from the buy and one from the sell, hence twice traded quantity. Per-order tests additionally assert `submitted_order = executed_order + cancelled_order + remaining_order` and no overfill.

Conservation alone does not prove price priority, FIFO, execution price, correct IDs, event ordering, or index validity. A wrong maker could still conserve quantity. Therefore the project combines directed examples, structural invariants, per-order ledgers, and differential tests against a simpler model. Each technique catches different classes of error.

## 12. What does the reference model contribute?

The reference chooses the next maker using a deliberately simple representation/selection process, rather than cloning the production map/list/index algorithm. Fixed-seed randomized streams compare acceptance, trades, complete resting state, and quantities after each operation for both allocator modes. A failure prints the seed and step so it can be reproduced.

An independent oracle reduces the chance that an implementation mistake is repeated in the test. It is still not a mathematical proof: the model can have bugs, inputs are finite, and rare resource failures may not be exercised. Directed boundary tests remain necessary, as do sanitizers for memory/undefined behavior and separate tests for network/queue semantics.

## 13. What optimization was tested, and why isolate it?

The book uses identical PMR maps, lists, and hash containers in both configurations. Only the resource changes from `new_delete_resource` to an engine-owned `unsynchronized_pool_resource`. The hypothesis is that chunk allocation and reuse reduce upstream allocations and perhaps improve locality. Cancellation bursts are an obvious case to test, while a monotonically growing book may retain more unused chunk space.

The pool does not prove fewer cache misses merely because it is faster, and it does not eliminate event-vector allocations. It can lose on setup costs or memory use. A meaningful answer cites the measured per-workload report, repeat spread, and instrumentation limits instead of promising a general speedup. The baseline stays available and remains the application default.

## 14. Why is allocation relevant to tail latency?

An allocation may take a fast local path most of the time but occasionally request new pages, encounter fragmentation, trigger a hash reallocation, or suffer cache/TLB misses. Such occasional costs affect upper percentiles more than the mean. Pooling can amortize some upstream requests but introduces chunk refill events and retains memory; it does not guarantee bounded latency.

The delivered benchmark uses a counting PMR upstream, forwarding both requested sizes and alignments, in a separate untimed pass. It measures peak outstanding book-resource **requested** bytes, not RSS. Ordinary result-event vectors and the pool resource object's own allocation are excluded from these counts. Timing passes have no counter wrapper; they include event-vector costs. Honest performance discussion distinguishes these scopes and avoids presenting book-resource metrics as total process memory.

## 15. What is the difference between throughput and latency here?

Throughput is incoming requests processed per second across one batch. Latency is the elapsed time for an individual engine request including destruction of its returned events, measured in a separate pass. Throughput is measured with one clock pair for the entire batch; latency has a pair per request, so timer overhead perturbs it. Its reciprocal mean is therefore not expected to equal independently measured throughput.

p50 is the median request; p95/p99 characterize upper tails. They are not maxima or guarantees. The summary reports the median of repeat p99s, not the p99 of all samples pooled together. A process descheduling pause, frequency shift, thermal throttling, or virtualized clock can affect tails. Core numbers exclude parsing, publication, queueing, TCP, disk, and console work, so they are not end-to-end exchange latency.

## 16. How does the bounded queue establish synchronization?

One mutex protects the deque, capacity condition, and closed flag. `push` waits on “closed or space,” and `pop` waits on “closed or nonempty.” Predicate waits handle spurious wakeups. The producer's writes before unlocking are visible to a consumer after it locks the same mutex; there is no need to sprinkle atomic memory orders around protected data.

The matching engine belongs only to the consumer. The queue is the ownership-transfer boundary for command strings. Full queues block producers instead of growing without bound; closure wakes both sides and lets existing items drain. Producer errors are read after `join`, which establishes synchronization. Closing the queue does not interrupt an input stream stuck in a blocking read, so shutdown assumptions must include the input source.

## 17. Would a lock-free SPSC ring automatically be better?

No. It may reduce lock/condition-variable overhead when one producer and one consumer are actively running, but it introduces wraparound, publication ordering, shutdown, full/empty ambiguity, and waiting strategy questions. A common scheme writes a slot then publishes the producer index with release; the consumer observes it with acquire before reading the slot. Consumption publication needs a corresponding reuse protocol.

Independent hot indices can false-share a cache line; padding/alignment can help but costs space and should be measured. Busy-waiting may improve low latency while wasting a core and increasing power/thermal pressure. The delivered mutex queue is the correct baseline; a ring is an optional future measured variant, not unfinished required functionality.

## 18. What does TCP guarantee, and what must the application add?

TCP supplies an ordered byte stream during a connection. It does not preserve send boundaries, define request/response framing, give exactly-once business execution, or recover application state after reconnect. Newline frames commands; `RESPONSE_END` frames each connection-local response. The server retains partial lines and partial-send offsets.

Per-client input/output bounds and read throttling implement backpressure; peers that exceed retained-output limits are disconnected. A client can lose its connection after its order was accepted but before reading the acknowledgement, so retry policy needs stable IDs and reconciliation. This simulator's persistent accepted-ID history helps reject a reused ID in the same session, but there is no durable recovery across process restart. UDP would additionally require handling packet loss, duplication, reordering, and gap recovery.

## 19. How should a subscriber apply market-data updates?

`MD_LEVEL` is an absolute replacement for a side/price level, not a quantity delta; quantity zero removes the level. Apply updates in market-data sequence order. The final `MD_TOP` and depth-limited snapshot describe final state after that changed request and reuse the latest level-update sequence. They do not represent new level changes.

Order acknowledgements describe requests, executions describe trades, and market data describes displayed remaining liquidity. Confusing them leads to double counting. If a subscriber misses a sequence, a snapshot is needed; silently continuing with incomplete increments gives an inconsistent book. A top-N snapshot cannot recover arbitrary deeper levels because it intentionally omits them.

## 20. Does TWAP guarantee execution of its parent quantity?

No. It guarantees a deterministic submission schedule subject to validation, not available liquidity. Quantity 23 in five children rounds to 5/5/5/4/4. Fixed limit orders may rest or reject. At the end, the simulator cancels remaining accepted children and reports submitted, accepted, rejected, executed, unfilled, and cancelled separately.

With only 13 units offered at or below a buy limit of 101, a submitted parent of 23 executes 13 and leaves 10 unfilled. Calling this “23 executed” because all children were sent would be a serious accounting error. External market commands at a child's timestamp run first; that tie policy makes the simulation reproducible but is not a claim about real network arrival order.

## 21. What remains between this project and a production exchange?

The simulator provides one instrument, limit orders/cancels, deterministic in-memory matching, education-scale TCP, bounded command queuing, market-data examples, and a deterministic TWAP client. A production venue additionally needs durable sequenced journals, recovery and failover, session management, authentication/authorization, risk controls, auditability, regulatory/business rules, precise protocol compatibility, operational monitoring, and carefully engineered latency/capacity limits.

It would also need adversarial load testing, capacity planning, resource-exhaustion policies, security review, tested disaster recovery, and a defined fairness model for ingress across clients. This implementation treats allocation/resource exhaustion as fatal rather than guaranteeing rollback after every possible exception. The right interview stance is to explain these boundaries and how you would extend them, without claiming that passing a test suite certifies an exchange.

## 22. How would you present the project in five minutes?

Start with one concrete sweep showing maker-price FIFO matching and a partial residual. Draw the two price maps, FIFO lists, and active-ID index. Explain single ownership, integer arithmetic, ID policy, and event sequences. Show an independent reference-model test plus a sanitizer verification record, distinguishing checks actually run from CI configurations.

Then show one recorded optimization comparison with hardware/compiler/workload metadata and its limitations. End with the bounded queue, TCP framing/partial I/O, publisher state consistency, and TWAP submitted-versus-executed accounting. Be prepared to trace a cancellation, quantify matching cost in terms of makers executed, and explain one negative performance result rather than citing only a favorable number.
