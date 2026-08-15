# Option contracts: timeouts, limits, and backpressure

The defaults are conservative boundaries, not workload guesses. Change an
option only when the application can explain the ownership, memory, and retry
consequence. The complete declaration and default values are in
[`options.hpp`](../../src/user/cpp/core/easy_uds/options.hpp).

## Timeouts

| Option | Scope | Contract |
|---|---|---|
| `connect_timeout` | Client | Maximum time to establish a one-shot or Session connection. `0` disables this connect-specific limit. |
| `io_timeout` | Client/server | Maximum interval between successful socket-I/O progress events. `0` disables inactivity expiry; it is not an absolute request deadline. |
| `request_timeout` | Client/server | Absolute fixed-request deadline. On the server it starts with the first header byte and includes queue wait, handler execution, and response write. `0` disables it. |
| `stream_timeout` | Client/server | Absolute stream-exchange deadline after the stream header arrives. `0` allows a long-lived stream, still subject to `io_timeout`. |
| `session_idle_grace` | Server | Worker continuation window for the Session fast path. `0` disables the optimization; it never changes request semantics. |

Deadlines do not forcibly interrupt a C++ handler or a blocking hardware call.
Use `RequestContext::stop_requested()` at safe interruption points and make the
operation idempotent or explicitly non-retryable before reconnecting. A queued
serialized request that reaches its server deadline is answered with `408` and
its handler is not called.

## Fixed-request admission

The per-connection limits protect the peer that is sending work:

| Option | Boundary | When it applies |
|---|---|---|
| `max_inflight_requests_per_connection` | Number of queued or executing fixed requests | The reactor stops reading that peer above the high-water mark and resumes below the low-water mark. |
| `max_inflight_request_bytes_per_connection` | Declared route+body bytes for queued/executing fixed requests | A request that cannot reserve its bytes remains under Unix-socket backpressure; the peer is not allowed to grow an unbounded parser queue. |
| `max_total_inflight_bytes` | Declared route+body bytes across all connections | Opt-in strict mode reserves after header validation and before parser allocation. Partial, queued, and executing requests share the same logical budget. `0` preserves the fast path and disables this aggregate cap. |

These are logical admission bytes, not a promise about kernel socket buffers or
application-owned bodies. A peer may still hold bytes in the kernel; stopping
`EPOLLIN` is what makes that pressure visible to the sender.

## Response output and slow peers

`max_output_bytes_per_connection` bounds unsent fixed-response wire bytes for a
single connection. When a peer stops reading and exceeds this cap, the server
closes that peer rather than keeping a worker blocked in `write()`. The opt-in
`max_total_output_bytes` budget provides the corresponding aggregate bound.
Output is drained by the reactor's `EPOLLOUT` path; handler execution does not
wait for a slow peer's socket buffer to become writable.

## Streams and memory

`max_message_size` limits a fixed route+body and fixed response body. Stream
payloads use `stream_chunk_size` for the reusable transfer buffer and
`max_stream_size` for the cumulative body; a stream limit of `0` means
unbounded-by-size, not unbounded-by-stall. `io_timeout` remains the protection
against a peer that makes no progress.

## Diagnostics and safe tuning

`Server::stats()` exposes the retained input/output bytes and queue depths that
correspond to these limits. Snapshots are best-effort, not transactional. Tune
one boundary at a time, run the stalled-peer and long-lived-session tests, and
compare RSS/p99 before changing several caps together. See the
[production diagnostics guide](../guides/production-diagnostics.md).
