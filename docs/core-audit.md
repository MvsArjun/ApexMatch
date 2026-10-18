# Core engineering audit

Audit date: 18 October 2026. This review inspected the existing engine, protocol,
public headers, independent reference model and core tests. It preserved the
ordered maps, FIFO lists, active-ID index, permanent accepted-ID registry,
single-owner API and optional PMR allocation policy.

## Initial classification

| Component | Classification before corrections | Basis |
|---|---|---|
| Limit insertion, bid/ask depth and maker-price matching | Verified working in exercised paths | Existing focused cases and independent randomized oracle; source inspection confirmed both incoming sides. |
| FIFO priority, partial makers, multiple fills and residual insertion | Verified working in exercised paths | Arrival sequences stay attached to FIFO nodes; targeted cases exercise multiple levels and partially filled makers. |
| Cancellation, duplicate IDs and rejected-ID reuse | Verified working in exercised paths | Active lookup plus list iterator erase; accepted IDs remain in the separate lifetime registry. |
| Quantity arithmetic | Verified working in exercised paths | Existing `INT64_MAX`/`UINT64_MAX` cases; lifetime accepted quantity cap bounds all aggregates. |
| Sequence exhaustion | Implemented but not fully tested | Conservative preflight existed; no practical public path fast-forwards an engine to exhaustion. |
| Baseline and pooled book ownership | Verified working in exercised paths | Allocator-equivalence tests and member-destruction review; injected allocation failures were a coverage gap. |
| Typed public enum arguments | Defective or inconsistent | Invalid `EventKind` indexed past a fixed array; other typed entrypoints silently selected a default behavior. |
| Parser and deterministic JSON replay | Verified working in exercised paths | Strict integer/record tests and allocator-equivalent replay; malformed parsed commands already rejected normally. |
| Exact randomized rejection reasons | Implemented but not fully tested | Oracle compared rejection kind and ID, but only required a nonempty actual reason. |

The classifications describe exercised behavior, not a formal proof. Whole
application builds, replay integration, extensions and platform-specific checks
are recorded in [verification.md](verification.md).

## GCC standard-library failure discovered during final verification

Fresh native w64devkit GCC 16.2 Debug and Release runs failed the C++ group with
a segmentation fault in the new allocation-failure case. GDB localized it to
`std::pmr::unsynchronized_pool_resource::do_allocate` on the first accepted-ID
insertion with a two-allocation upstream allowance. It occurs before any
accepted order or book node exists. The ordinary replay, network, example and
Python tooling groups passed in those runs; the combined C++ group did not.

[repro_pool_failure.cpp](../tools/repro_pool_failure.cpp) reproduces the failure
using only the standard library, without ApexMatch headers or engine code:

```sh
g++ -std=c++20 -O0 -g tools/repro_pool_failure.cpp -o pool_failure_probe
./pool_failure_probe
```

On the tested runtime it allocates 528 bytes for bookkeeping, 1016 for a chunk,
and attempts 192 for chunk metadata. The injected third-call `bad_alloc` causes
the 1016-byte allocation to be released and then a segmentation fault instead
of propagating to the caller. Those sizes are runtime-specific, not a required
allocator contract.

The [GCC 16 branch's pool implementation](https://github.com/gcc-mirror/gcc/blob/releases/gcc-16/libstdc%2B%2B-v3/src/c%2B%2B17/memory_resource.cc)
supports the diagnosis: its replenish exception handler releases the new chunk
without rethrowing; its caller then uses the chunk-vector tail even when that
vector is empty. This is an inference from the source corroborated by the
engine-free local reproducer and GDB stack. The tested Clang/libc++ runtime
passed the complete failure-injection case.

The application regression remains enabled. No assertion was weakened and no
allocator was replaced to conceal the dependency failure. Full GCC C++ testing
on this runtime therefore remains failed, while Linux GCC/Clang runtimes still
need their own execution. Generalizing this result to every GCC version or
platform would be unsupported. See [verification.md](verification.md) for the
final platform matrix and preserved failure evidence.
The standalone reproduction procedure and full dependency diagnosis are in
[toolchain-issue.md](toolchain-issue.md).

## Reproduced defect and corrections

A direct call to `event_json` with
`event.kind = static_cast<EventKind>(99)` reproduced this pre-fix diagnostic
under native Clang ASan/UBSan:

```text
src/protocol.cpp:89:30: runtime error: index 99 out of bounds for type 'const char *const[5]'
SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior src/protocol.cpp:89:30
```

The serializer now checks the kind before indexing. The ordinary text parser
could not construct this value; the defect affected the public typed API.

The audit also found that `side_name` and `quantity_at` treated unknown typed
`Side` values as sell, an unknown `AllocationMode` silently selected baseline,
and an unknown `CommandKind` silently produced no result. These APIs now throw
`std::invalid_argument` for unknown enum values. An invalid side passed to
`MatchingEngine::add` retains its established business-rejection behavior.
Unknown event kinds and side-bearing serialized events have focused regression
coverage. Invalid command kinds preserve engine state and counters.

No matching-rule defect was found in the inspected implementation or exercised
test streams. No container, matching algorithm or normal event order was
replaced.

## Ownership and lifetime findings

The price maps own `Level` values; each level owns its FIFO `Order` nodes. The
active hash index holds iterators into those lists. Hash rehashing therefore
does not invalidate its referenced list nodes, and map insertion does not
invalidate existing levels. Cancellation removes the active entry and FIFO
node; an empty level is erased only after its last order is removed. Matching
copies trade fields before removing a completed maker.

The engine is not copyable or movable through its noncopyable book. Members
are destroyed in reverse declaration order: both hash containers and both maps
are destroyed before the pool. The externally supplied upstream resource is
non-owning and must outlive the engine. The pool is unsynchronized and shares
the engine's single-owner requirement.

Allocation failure remains a fatal session error. Mutation is not transactional:
an allocation can fail after statistics, IDs or some fills change. Callers must
discard the engine after such a failure. The added resource-failure test checks
safe destruction and release of all injected upstream book allocations; it
does not claim rollback or cover every global `new`, such as result vectors.

## Arithmetic and sequencing findings

Prices are compared rather than subtracted. Executed quantity is the minimum
of positive signed remainders, so each subtraction is nonnegative. The lifetime
accepted submitted-quantity cap is checked with subtraction before insertion;
it bounds a level aggregate, all resting quantity, total cancelled quantity,
and twice traded quantity. Cancelling does not replenish that budget. No
notional multiplication is performed.

The original sequence preflight was extracted without changing its policy into
the internal [sequence_capacity.hpp](../src/sequence_capacity.hpp) predicate.
It rejects exhausted request sequence, unsafe active-count multiplication,
insufficient space for `2 * active_orders + 3` events, or insufficient execution
space. Boundary tests invoke this actual predicate at valid and invalid
`UINT64_MAX` limits. They do not inject near-exhausted counters into a live
engine; this distinction matters when describing coverage.

## Added verification

Three core named cases were added:

1. Typed enum and null-resource validation, including unchanged engine state.
2. Sequence preflight boundary decisions, including multiplication headroom.
3. Allocation-failure destruction, with 64 successive upstream allocation
   allowances for each allocation mode and an outstanding-byte check after
   every session is destroyed.

The independent flat-vector oracle now supplies exact rejection reasons and
the comparator checks them on all 40,000 deterministic requests. It still uses
linear maker selection and cancellation rather than production containers or
the production validator.

The isolated native Clang release core run passed **25 named cases**, including
all **40,000 differential requests**, with `-O2 -DNDEBUG` and the project's
warning set. An isolated `-O1 -g -fsanitize=address,undefined` core run passed
the same 25 cases with no sanitizer diagnostics; `detect_leaks=0` was used on
Windows, so this is not a LeakSanitizer result. Consult
[verification.md](verification.md) for final combined build and sanitizer
results. Near-exhaustion engine state injection, global
allocation-failure rollback and formal proofs remain outside this coverage.
