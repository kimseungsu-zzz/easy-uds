# Runtime statistics

`Server::stats()` and `Session::stats()` return bounded, thread-safe snapshots
for operational diagnosis. They do not start a background sampler, publish
metrics, or make RPC execution wait for an observer.

## Server gauges

Operational gauges are always available:

| Field | Accounting boundary |
|---|---|
| `running` | Whether `run()` is currently accepting/reacting to work. |
| `active_connections` | Entries retained by the server connection table. A closing connection with dispatched work remains counted until that work releases its descriptor reference. |
| `active_streams` | Stream exchanges that currently own a stream slot. |
| `inflight_requests` | Fully admitted fixed requests that are queued or executing. Streams are represented by `active_streams`. |
| `retained_request_bytes` | Logical request bytes already tracked for backpressure. In strict aggregate-budget mode this also includes admitted partial frames; stream chunks are not retained and are excluded. |
| `queued_output_bytes` | Unsent fixed-response wire bytes in reactor output queues. |
| `worker_queue_depth` | Fixed and stream jobs waiting in the normal worker queue; executing jobs are excluded. |
| `serialized_queue_depth` | Requests and maintenance tasks waiting across all serialization domains; executing items are excluded. |
| `active_serialized_domains` | Serialization domains currently executing a request or maintenance task. |

These fields reuse accounting required by connection limits, backpressure, and
executor operation. Calling `stats()` briefly takes the connection, worker,
and serialized bookkeeping mutexes one at a time. It does not hold more than
one of them simultaneously and does not wait for a handler or socket I/O. The
cost is O(active connections) plus three constant-time queue snapshots.

The fields are individually race-free but are not one transactional instant:
a request may move from a queue to execution between two fields being sampled.
Use a sequence of snapshots for trends and limits, not for an invariant such
as `queue + executing == total` across every field.

## Optional server counters

Cumulative event recording is disabled by default:

```cpp
easy_uds::ServerOptions options;
options.stats = easy_uds::StatsMode::basic;
easy_uds::Server server("/run/robot.sock", options);
```

When disabled, `ServerStats::counters` is empty and a normal request performs
no statistics atomic read-modify-write. When `basic` is enabled, the optional
contains:

| Counter | Exact meaning |
|---|---|
| `accepted_connections` | Connections successfully inserted into epoll and the server connection table. |
| `rejected_connections` | Accepted sockets immediately closed because `max_connections` was full. |
| `fixed_requests_dispatched` | Complete fixed frames handed to normal or serialized dispatch, including 404 routes. |
| `stream_requests_started` | Complete stream openings that acquired a stream slot. |
| `stream_requests_rejected` | Complete stream openings closed because no stream slot was available. |
| `requests_timed_out_before_execution` | Fixed requests whose server deadline elapsed before their normal or serialized handler began. |
| `serialized_requests_superseded` | Queued `LatestWins` requests answered with 409 because a newer request with the same domain and concrete route arrived. |
| `serialized_requests_rejected_busy` | `RejectIfBusy` requests answered with 409 because their domain was executing or already had queued work. |

The common enabled fixed-RPC path increments one cache-line-separated,
thread-assigned relaxed counter shard. Rare rejection and timeout paths update
their specific counter. These counters are
diagnostic observations, not billing or durable audit records; they reset when
the `Server` is reconstructed.

## Session statistics

`Session::stats()` always reports in-flight depth for fixed requests on the
multiplexed persistent connection. Cumulative outcomes use the same explicit
mode on the `ClientOptions` that creates the Session:

```cpp
easy_uds::ClientOptions options;
options.stats = easy_uds::StatsMode::basic;
easy_uds::Session session = easy_uds::Client(path, options).session();
```

When disabled, `SessionStats::counters` is empty. When enabled it contains:

| Field | Meaning |
|---|---|
| `requests_started` | Requests registered in the in-flight table. Local validation failures and calls rejected after the Session is already broken are excluded. |
| `requests_completed` | Responses received successfully, regardless of application status such as 404 or 500. |
| `requests_timed_out` | Calls that reached the client-side absolute request deadline. |
| `requests_failed` | Registered calls ending in transport, protocol, or connection failure. |

Dedicated `Session::request_stream()` connections are intentionally excluded.
Each enabled in-flight shard updates ordinary integers in a separate counter
array while holding the shard mutex that request correlation already needs.
The allocation is made once when the Session is constructed; it does not grow
the cache-line-rounded in-flight shard and adds no mutex or atomic RMW.

Snapshot cost is O(16 shards in the default build). Shards are locked one at a
time, so callers remain concurrent and the returned fields have the same
best-effort, non-transactional semantics as server statistics. Calling
`stats()` on a moved-from Session throws `std::logic_error`.
