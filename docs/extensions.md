# Pipeline, market data, local networking, and simulated TWAP

The four extensions use the same `MatchingEngine`, parser, integer prices, validation, ID policy, and event format as replay. The applications are educational simulations. They do not connect to a broker or exchange.

Source entry points: [`extensions.hpp`](../include/apexmatch/extensions.hpp), [`extensions.cpp`](../src/extensions.cpp), [`pipeline.cpp`](../apps/pipeline.cpp), [`server.cpp`](../apps/server.cpp), [`client.cpp`](../apps/client.cpp), and [`twap.cpp`](../apps/twap.cpp). See [`extensions_tests.cpp`](../tests/extensions_tests.cpp) and [`network_test.py`](../tests/network_test.py) for executable examples of the guarantees described below.

## Bounded producer-consumer replay

```sh
./build/release/apex_pipeline --capacity 64 examples/sample.txt > pipeline.jsonl
./build/release/apex_cli examples/sample.txt > sequential.jsonl
cmp pipeline.jsonl sequential.jsonl
```

The producer reads input and pushes newline records into `BoundedQueue<std::string>`. The calling thread consumes each record and owns the engine. The producer never accesses the book. Standard output contains only deterministic replay JSON. Standard error reports the number of input lines, the observed queue high-water mark, and capacity. The high-water mark depends on thread scheduling; it is deliberately separate from deterministic output.

`BoundedQueue<T>` owns its queued values. A successful push transfers its value into a FIFO deque. `pop()` moves one value out. The queue supports multiple producers and consumers, although this application uses one of each. With several producers, the push order is the order in which pushes acquire the mutex and commit; it does not impose an external ordering on independent sources. The single producer preserves file order exactly.

A mutex protects queue contents, closure, and high-water accounting. Condition variables wait with predicates, handling spurious wakeups. A full queue blocks producers, applying backpressure instead of growing the queue. An empty queue blocks consumers. The same mutex unlock/lock synchronization establishes visibility of constructed values: there are no explicit relaxed atomics or hand-written acquire/release operations to explain away. Closing is idempotent, prevents new pushes, and wakes blocked readers and writers. Existing values remain available until drained, after which `pop()` returns `nullopt`.

The queue must outlive all operations on it. The pipeline joins its producer before destroying the queue. A producer exception is stored and rethrown after draining and joining. A consumer/output exception closes the queue, wakes a producer blocked on a push, joins it, and propagates the error. Closing a queue cannot interrupt a generic input stream blocked inside the operating system; use finite files for unattended replay, and finish or close stdin when running interactively.

Producer storage retains at most 4097 bytes per record. The parser limit is 4096: the extra byte is an oversized-record sentinel. Remaining bytes of an oversized line are discarded until its newline, producing exactly one `line_too_long` rejection. A final nonempty line is processed at EOF even without a newline. Thus queue record storage is bounded by `capacity * 4097` bytes plus container/string overhead. Blank records count as input lines but produce no engine events.

This is the deliberately simple synchronized baseline. A mutex and condition-variable implementation is a better starting point than a casually implemented lock-free ring. No SPSC ring or false-sharing optimization is claimed here. A ring experiment would require ownership of head/tail indices, carefully justified publication ordering, separate cache lines for independently written indices, stress tests, and measurements against this baseline.

## Market-data semantics

Order entry (`ADD`, `CANCEL`) asks to change the engine. Accepted/rejected acknowledgements answer that request. Execution events report matched quantity. Market data describes the resulting visible resting book and has an independent sequence.

`MarketDataPublisher` consumes the engine's `BookUpdate` events and publishes:

| Message | Meaning |
| --- | --- |
| `MD_LEVEL` | Absolute replacement for one `(side, price)` aggregate. `quantity:0` and `orders:0` delete that level. It is not a quantity delta. |
| `MD_TOP` | Final best bid/ask after the entire command, with `null` for an empty side. |
| `MD_SNAPSHOT` | Authoritative current top-N depth checkpoint, nested in `depth`; bids descend, asks ascend. |

Every `MD_LEVEL` increments `md_sequence` once and carries the originating `request_sequence`. The final `MD_TOP` and `MD_SNAPSHOT` repeat the last incremental sequence; these checkpoints do not allocate another sequence. Initial snapshots use the current sequence, zero before any book changes. Rejections and fully executed takers that cause no additional resting level do not invent extra book updates; maker changes still generate incrementals normally.

One command can generate several `MD_LEVEL` messages, including several replacements at the same level during a FIFO sweep. Applying them in order produces the final level state. `MD_TOP` and the snapshot are published only after all incrementals for that command, so they always describe the final book. The network application emits the requester's engine events first, then market data to every subscribed connection, then the requester's `RESPONSE_END`. Another subscriber receives only its own request responses and market data, not other clients' private acknowledgements/execution responses.

The default snapshot has ten levels per side. Incrementals describe all changed levels, including changes below those ten. A top-N initial snapshot cannot reconstruct unknown deeper levels by itself; the per-command snapshots are authoritative for top-N consumers. `publish_to` streams individual messages to a sink so networking does not construct a second arbitrarily large combined response buffer. The convenience `publish` method concatenates the same messages for tests/offline callers.

## Local TCP server and client

Run these in two Linux/WSL terminals after building:

```sh
# Terminal 1
./build/release/apex_server --port 9000

# Terminal 2: subscribes, submits orders, queries, and shuts the server down.
./build/release/apex_client --port 9000 examples/network.txt
```

The listener always binds to `127.0.0.1`; there is no remote-host option. `--port 0` requests an available ephemeral port, printed as `LISTENING 127.0.0.1:<actual-port>`. This is used by integration tests. The native Windows build uses Winsock; Linux/WSL uses POSIX sockets. CMake links the platform dependency automatically.

The wire format is newline-delimited text requests and newline-delimited JSON replies. Requests use exactly the replay grammar, plus three transport controls:

```text
SUBSCRIBE
UNSUBSCRIBE
SHUTDOWN
```

`SUBSCRIBE` enables market data for that connection and immediately returns an `MD_SNAPSHOT`. `UNSUBSCRIBE` disables future market data and returns `UNSUBSCRIBED`. `SHUTDOWN` is a simulator control, acknowledged with `SHUTDOWN`, and closes the listener immediately. Control commands allocate no engine request sequence. A transport control may end in CRLF. Replay order-entry commands share the parser's whitespace handling. There is no authentication, encryption, session recovery, or durable order journal; this protocol is for local simulation.

Each connection initially receives:

```json
{"type":"HELLO","protocol":"apexmatch-json-lines-v1"}
```

Each complete input line, including an ignored blank/comment line, ends its response with:

```json
{"type":"RESPONSE_END","command_sequence":1}
```

`command_sequence` starts at one independently for each connection and counts transport lines. It is distinct from the engine's globally ordered mutation request sequence. This marker lets the bundled client distinguish a command producing many execution events from one producing no engine events. Subscribed clients must also accept unsolicited market-data messages between responses. Requests within one connection retain receive order. Multiple connections are serialized by one event-loop owner; OS arrival timing across clients is not a deterministic replay input. Once a particular ordered request trace is recorded, engine results are deterministic.

TCP is a byte stream. One write can arrive in several reads, or many commands can arrive in one read. The server retains incomplete lines and processes every completed frame in order. Every send records its actual byte count; unsent suffixes remain queued for later writability. Would-block and interrupted system calls do not lose bytes. The bundled blocking client also loops over short sends, retains partial receive lines, waits through `RESPONSE_END`, and uses five-second send/receive timeouts.

Limits and backpressure are explicit:

| Option | Default | Behavior |
| --- | ---: | --- |
| `--max-line` | 4096 bytes | Oversized transport frame receives one `line_too_long` rejection and response marker, then the connection closes after draining. Maximum configurable framing bound is 1 MiB; the shared command parser still limits normal commands to 4096 bytes. |
| `--max-output` | 1048576 bytes | Per-connection pending output bound. Reads pause while pending output is at least half the bound. If a message would exceed the hard bound, the slow peer is disconnected. Minimum is 256. |
| `--max-clients` | 32 | Limits established connections handled by the event loop. Must be positive and below the platform `FD_SETSIZE`. |

One bounded 4096-byte receive occurs per readable connection per loop, helping avoid starvation. Input storage never exceeds the configured frame limit. Each connection's pending application output is bounded; socket-kernel buffers are additional OS-managed memory. Temporary engine results still cost O(number of executions): the networking queue bound is not a cap on book size or one command's matching work. A dropped peer may have submitted a command that executed before its acknowledgement was dropped. This simple protocol does not provide exactly-once recovery after disconnects; clients must not assume missing acknowledgements imply rejection.

A receive EOF marks the input side closed while queued replies drain. A final unterminated fragment produces `unterminated_line`; unlike finite file replay, a network command requires its delimiter. Abrupt close/reset affects only that peer. `SHUTDOWN`, SIGINT, or SIGTERM closes the listener, stops processing new commands, and drains already queued output. A two-second deadline prevents an unresponsive peer from hanging shutdown. RAII closes sockets on normal completion and exceptions. The Windows process-termination integration test can only verify forced termination; Windows `TerminateProcess` does not deliver POSIX SIGTERM. The explicit `SHUTDOWN` path is verified on both platforms.

UDP is deliberately omitted. Datagrams preserve message boundaries but can be lost, duplicated, or reordered and provide no built-in retransmission or connection backpressure. A realistic UDP market-data channel would need sequence checking, gap handling, snapshots/recovery, and explicit loss policy. Replacing TCP sends with UDP sends would not preserve this interface's ordering and delivery behavior.

## Deterministic TWAP client

```sh
./build/release/apex_twap --quantity 23 --slices 5 --start 0 --end 100 \
  --side BUY --price 101 --first-id 1000 --market examples/twap_market.txt
```

`apex_twap` is a client of the in-process matching API. It uses the same matching infrastructure as the CLI/server without sleeping or depending on wall-clock timestamps. Network transport is a separate interface and is not used to decide simulation time.

The parent submits 23 units through five child limit orders. The limit-price policy is constant: every child uses `--price`. Market orders, adaptive quoting, participation-rate logic, and execution guarantees are outside this simulator's scope.

For `n > 1`, child `i` is scheduled at `start + floor(i * (end-start)/(n-1))`; both endpoints are included. For a single slice it is scheduled at `start` and can remain available until `end`. Zero-duration schedules are allowed and submit all slices at the same simulated time. Integer quantity rounding gives every child `quantity/n`, with one extra unit assigned to the first `quantity%n` children. Zero-quantity children are forbidden, so slices must be between one and `min(quantity,1000000)`.

Time and child IDs are unsigned 64-bit integers; price and quantity use the core's signed 64-bit positive values. The scheduler uses a quotient/remainder decomposition to avoid multiplying a potentially maximal duration by the child index. It validates the entire child-ID range before submitting anything. Sequential IDs start at `--first-id`. Existing/used IDs follow the engine's normal duplicate-rejection policy; a rejected child is reported and is never cancelled as if it were owned by this strategy.

Timed market files use `<simulation-time> <ADD-or-CANCEL-command>`. Blank and comment lines are allowed. Events must be within the parent interval; input is stably sorted by time. External events run before child submissions at equal timestamps, retaining file order among ties. This tie policy defines reproducible available liquidity. Events at `end` and the final child run before terminal cancellation. The simulator cancels every remaining accepted child at `end`; independent market orders remain in the book. External cancellation of a child is also counted in the strategy's cancellation report.

The example schedules child quantities `[5,5,5,4,4]` at times `[0,25,50,75,100]`. Eight units of sell liquidity at 100 and five at 101 fill 13 parent units. Sell liquidity at 102 is above the buy limit and remains untouched. The remaining ten parent units are cancelled. Each event is wrapped with its deterministic simulation time; the final line is:

```json
{"type":"TWAP_REPORT","children":5,"completion_time":100,"submitted_quantity":23,"accepted_quantity":23,"rejected_quantity":0,"executed_quantity":13,"unfilled_quantity":10,"cancelled_quantity":10}
```

This final line is also committed in [`twap.expected-report.json`](../examples/twap.expected-report.json). Submitted quantity counts attempted children; accepted and rejected quantities separate their outcomes. Executed quantity counts actual child fills, including fills caused by later market events. Unfilled quantity is accepted minus executed. Cancelled quantity includes terminal and external child cancellations. Completion means the schedule and terminal cleanup finished; it never implies the parent filled.

## Verification and interview discussion

```sh
ctest --preset debug --output-on-failure
python3 tests/network_test.py --server build/debug/apex_server --client build/debug/apex_client
```

The C++ extension groups verify capacity and FIFO, blocked writer/reader wakeup on close, draining, 50,000 ordered values, two simultaneous producers without loss/duplication, replay equivalence at several queue capacities, oversized-record discard, EOF, output failure, market-data sequence/depth agreement, TWAP rounding/timing, later fills, external cancellations, duplicate-ID collisions, deterministic replay, and arithmetic boundaries.

The portable TCP test verifies one-byte request fragments, several commands in one write, malformed input, exact line limit and over-limit closure, partial-fill and multi-level market-data reconstruction from the actual emitted JSON, equality with snapshots and `BOOK`, half-close/trailing fragments, abrupt disconnects, delivered client behavior, a deterministic hard output-bound disconnect, continued service to a healthy client, and clean explicit shutdown with an idle subscriber. A separate 600-maker sweep produces a response larger than the server's 65536-byte send limit and the client's 4096-byte receive buffer, exercising queued send offsets and reply framing across multiple reads; it checks all 600 positive FIFO executions, response delimiters, and the final empty book. POSIX additionally verifies SIGTERM draining. Run the same C++ tests under the provided Linux ASan/UBSan and separate TSan configurations; consult [`testing.md`](testing.md) for the recorded environment-specific results and limitations.

[`extensions_e2e.py`](../tests/extensions_e2e.py) also launches the delivered pipeline and TWAP executables. It compares pipeline file/stdin output with the committed replay golden file, verifies bounded oversized-record recovery, and checks both the default and explicit TWAP fixtures against the committed report and repeated output.

Useful interview questions: Why does a bounded queue matter when matching is slower than input? Which mutex operations publish constructed values? Why does closing differ from clearing? Why does a snapshot carry the latest incremental sequence instead of allocating a new one? Why can a TCP acknowledgement be lost after an accepted order? Why is cross-client wall-clock arrival not a replay order? Why does a TWAP completion report need separate submitted and executed quantities? Answer each by following the corresponding test into the implementation.
