# Presenting ApexMatch in an interview

These scripts describe the implemented project. Replace performance statements
with numbers from an actual recorded run and name its platform; use
[verification.md](verification.md) for current build/test evidence. Do not
describe configured Linux CI as a completed hosted run.

## Thirty-second introduction

“ApexMatch is a C++20 limit-order matching simulator for one instrument. It
matches by best price, then arrival time, and executes at the resting order's
price. I used ordered price maps, FIFO lists and an ID index so the ownership
and cancellation rules are easy to audit. I checked it against an independent
reference model over 40,000 deterministic requests. The project also compares
ordinary and pooled allocation, with explicit latency and memory tradeoffs.”

## Two-minute walkthrough

“The core accepts limit orders and cancellations and returns sequenced events.
A buy visits the lowest eligible ask; a sell visits the highest eligible bid.
At one price, the FIFO front trades first. A partially filled maker keeps its
original position, and a positive incoming remainder rests after matching.

“Each side has an ordered price map. Each level owns a FIFO list of orders,
and a hash index maps an active ID to its list iterator. That makes cancellation
a hash lookup, a map lookup and a list erase. A separate accepted-ID set keeps
IDs retired after fill or cancellation. It is a deliberate session rule and a
persistent memory cost.

“The engine has one owner. A bounded queue demonstrates producer/consumer
ingress, and a separate localhost TCP interface demonstrates framing, partial
I/O, bounded replies and market-data subscriptions. A simulated TWAP client
uses the same engine with integer time; it does not connect to a broker.

“Correctness is supported by targeted cases, invariant checks and an independent
vector-based reference engine. The randomized comparison checks complete event
order, trades, depth, order contents and statistics. Quantity conservation is
also checked per order. The audit found an invalid-enum serialization bug,
reproduced it under UBSan and added a targeted correction and regression.

“For performance, the optional PMR pool changes allocation policy while retaining
the same matching logic. Benchmarks separate batch throughput, sampled latency
and memory counts. I would show the recorded platform and repeat spread before
discussing any result. Pooling can trade reduced allocation work for retained
memory, so baseline allocation remains the default.”

## Five-minute technical deep dive

“I would start with a small trace. Suppose order 1 sells 5 units at 100 and
order 2 sells 7 at 100. A buy for 8 at 101 first executes 5 against order 1,
then 3 against order 2, both at 100. Order 1 is removed; order 2 stays at the
front with 4 remaining and its original arrival sequence. Cancelling order 2
removes those 4 units, not its original 7. That one trace illustrates price
eligibility, maker pricing, FIFO, partial fills and cancellation accounting.

“The representation follows those rules directly. Ordered maps expose the
best bid or ask at their endpoints without sorting the book on every request.
Each level owns a linked FIFO list. An active hash index stores the order's
side, price and list iterator. The iterator refers to a list node, so rehashing
the index cannot move the order. Empty levels are erased only after the last
order leaves. A separate set retains accepted IDs forever within the session,
so a completed order's ID cannot become a new order by accident.

“These choices have costs. Trees and lists allocate and chase pointers; a
bounded tick ladder or intrusive representation might have better locality
under different constraints. Cancellation is expected O(log L), because it
still looks up a price map after the hash lookup. A sweep touches K makers;
absolute book updates currently perform map lookups, making the implemented
cost O(K log L + log L) under average hash behavior, plus result allocation.
I would not call the whole engine constant time.

“The engine mutates on one owning thread. That avoids races among FIFO nodes,
aggregate quantities, indexes and the unsynchronized pool. The bounded queue
uses a mutex and predicate-based condition variables. Closing prevents new
pushes, wakes waiters and lets the consumer drain. Consumer initialization
finishes before the producer starts, and failure closes the queue before a
join, avoiding a producer stuck behind a full queue with no consumer.

“The network server is another adapter with a single event-loop owner. Newline
frames commands, receive buffers retain partial records, and send offsets
retain unsent bytes. A response-end marker groups multi-event command replies.
Per-client output limits disconnect slow peers and reads pause before that
limit. A disconnect can lose an acknowledgement after execution; the protocol
does not provide persistence, reconnect reconciliation or exactly-once recovery.
Those are explicit simulator limits.

“The arithmetic policy is conservative. Individual prices and quantities are
positive signed 64-bit values, but aggregates are unsigned 64-bit. Before
accepting quantity, the engine checks the lifetime submitted sum against
UINT64_MAX using subtraction, so no aggregate can wrap. Trades consume two
sides, giving submitted = twice traded + cancelled + resting. Sequence
headroom is checked before mutation for the largest possible event count.
Unexpected allocation failure is fatal to a session; there is no promised
transactional rollback. I test safe destruction after injected book allocation
failures and do not resume those engines.

“For evidence, the reference engine is deliberately slow and different: one
unsorted vector with linear best-maker selection and cancellation. Eight fixed
seed/allocation combinations exercise 40,000 requests. After every request,
tests compare events, exact trades and rejection reasons, depth, individual
orders, arrival priority and statistics, then run invariants. Focused tests
cover maximum integer values, middle-FIFO erasure, invalid input and exact event
sequences. The final verification record distinguishes actual sanitizer runs
from unavailable checks.

“Finally, the pool experiment changes only book allocation. Throughput, latency
and memory use separate passes over the same workload. Core event creation is
included; parsing, JSON and network I/O are excluded. Requested book bytes are
not RSS and exclude event vectors. I would present current repeat ranges and
memory alongside throughput, then explain which additional profiles would
justify another change. I would not infer cache behavior or production latency
from a laptop benchmark.”

## Source tour to accompany the explanation

1. [`engine.hpp`](../include/apexmatch/engine.hpp): show value types, level
   ownership, FIFO iterators and the single mutation API.
2. [`MatchingEngine::add` in engine.cpp](../src/engine.cpp): trace the two fills
   above, then open `cancel` and the conservation checks.
3. [`reference_model.hpp`](../tests/reference_model.hpp) and
   [`randomized_reference`](../tests/core_tests.cpp): explain why the oracle
   algorithm is different and what each request compares.
4. [`BoundedQueue`](../include/apexmatch/extensions.hpp) and
   [`replay_pipeline`](../src/extensions.cpp): explain predicate waits, close,
   ownership, initialization and failure order.
5. [`run_local_server` and `simulate_twap`](../src/extensions.cpp): choose one
   adapter in depth instead of listing every extension superficially.
6. [`batch`, `latencies` and `memory`](../benchmarks/benchmark.cpp): show the
   actual measurement boundaries, then the recorded results and limitations.

## Architecture decisions worth defending

| Decision | Benefit | Cost or constraint |
|---|---|---|
| Ordered price maps | Sparse arbitrary integer prices, clear best-price traversal | Tree traversal, node allocation and pointer chasing |
| FIFO lists and active-ID iterator index | Direct same-price priority and stable cancellation handles | Per-node allocation and poor contiguous locality |
| Separate accepted-ID registry | Unambiguous duplicate policy after completion | Memory grows with accepted session history |
| Single-owner matching | Straightforward sequencing, iterator safety and unsynchronized pool use | Ingress and queries must respect the owner; one core processes the book |
| Integer prices and conservative lifetime quantity cap | Auditable arithmetic and reproducibility | Finite session budget; no notional/P&L implementation |
| Returned event values | Deterministic adapters and independently testable event order | A sweep allocates O(K) result storage |
| Optional PMR pool | Same semantics with different allocation policy | Retained capacity and sometimes worse latency percentiles |

## Hard correctness problems to discuss

The most useful examples are partially filled makers keeping priority,
cancellation of a middle FIFO node, removing the final maker at a level,
retiring IDs independently of active state and ordering acknowledgement,
execution and absolute level-update events. Show concrete tests for each.
Explain why price comparison avoids signed subtraction and why aggregate
limits need more than per-order validation.

The audit's invalid-event-enum defect is also a good engineering example:
untrusted text parsing was strict, but a public C++ value could bypass that
parser and reach an unchecked array index. The fix validated the typed API and
added a sanitizer-reproduced regression. See [core-audit.md](core-audit.md).

Final native GCC verification also found a pool-resource dependency failure
under injected allocation exhaustion. The same crash was reproduced without
engine code and traced in GDB, while native Clang/libc++ propagated the exception
safely. Keep the GCC suite's failed status explicit and explain the isolation
process rather than claiming universal portability. The standalone source and
evidence are linked from [toolchain-issue.md](toolchain-issue.md).

## Performance discussion and next improvements

Present the existing allocator experiment as an experiment, naming the recorded
machine, compiler, flags, workload and repetitions. Historical results are
preserved in `results/native-clang23`; use [performance.md](performance.md) to
identify current reruns. Do not describe those numbers as a universal latency
target or present timing as proof of hardware-cache behavior.

Next work should follow evidence. Useful candidates are event-storage costs,
map lookups in update publication, and allocation/cache profiles for large
books. A production evolution would first need a sequenced durable journal,
recovery/session semantics, ingress fairness, risk controls, capacity limits
and operational testing. Those changes require new contracts and tests; they
are not cosmetic additions to a simulator. See
[interview-practice.md](interview-practice.md) for twenty deeper questions.
