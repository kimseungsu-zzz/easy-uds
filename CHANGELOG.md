# Changelog

All notable changes to this project are documented here.

## Unreleased — 0.6.3 Experimental Closure

- Added optional aggregate server budgets for queued fixed-request input and
  fixed-response output across all connections.
- Global input budgets participate in reactor pause/resume watermarks; global
  output budgets are released on flush or connection cleanup.
- Added validation and regression coverage for configured memory budgets.
- Added a build-time `EASY_UDS_SESSION_SPIN_US` knob for controlled latency
  experiments without expanding the public runtime API; the default remains
  `100` microseconds.
- Added `getrusage()` CPU/context-switch reporting to the session benchmark and
  standalone `SCM_RIGHTS`/`memfd_create` and `io_uring_setup` capability probes.

## 0.6.1 — Reactor Hardening

Patch release focused on bounded resource use, persistent-session reliability,
and lower hot-path overhead. The public API and protocol remain unchanged from
0.6.0.

### Reliability and backpressure

- Idle sessions no longer expire merely because no request is in flight; response
  deadlines still apply once a transaction starts.
- Fixed-response writes use a bounded per-connection output queue drained by the
  reactor, so non-reading peers cannot occupy the worker pool.
- Pipelined fixed requests are bounded per connection by request count and queued
  bytes. The reactor pauses and resumes `EPOLLIN` at high/low watermarks, applying
  Unix-socket backpressure instead of growing an unbounded worker queue.
- Added regression coverage for idle sessions, stalled readers, bounded output,
  pipelined input backpressure, and low-watermark resumption.

### Performance

- Session completion wakes only the waiter for the completed request rather than
  broadcasting to every in-flight request.
- Session in-flight map nodes and reactor connection state are reused on hot paths.
- Immutable copy-on-write handler snapshots remove handler-table locking and
  `std::function` copies from request dispatch.

### Internals and validation

- Split the reactor into parser, event loop, dispatch, flow-control, output,
  streaming, and worker-executor modules under `src/reactor/`.
- Split the unit suite by subsystem under `tests/easy_uds_test/`; all C/C++ source
  files are now below 600 lines.
- Validated with GCC and Clang static/shared builds, 37 unit tests, stress tests,
  ASan/UBSan, TSan, protocol/session fuzz smoke tests, and installed-package
  consumers.

## 0.6.0

Breaking rewrite that reshapes the protocol, server internals, and public API before 1.0.

### Protocol version 2 (breaking, not wire-compatible with 0.5.x)

- 20-byte big-endian header adds a `request id` field for multiplexing; version byte is now `2`. A v2 server rejects v1 connections.
- Fixed requests on a connection may now be pipelined: the client picks an id, the server echoes it in the response, and responses may arrive out of order. `Client::session()` multiplexes concurrent `request()` calls.
- Clients reject unknown or mismatched response ids as protocol errors instead of accepting or silently dropping them.
- Stream frames carry the stream's request id. Streams remain exclusive per connection (half-duplex), and `Session::request_stream()` uses its own dedicated connection.
- A request that waits past the server-side `request_timeout` before a worker executes it is answered with `408` instead of being silently dropped.

### Server: epoll reactor (breaking internals)

- The listening/accepting and per-connection frame parsing moved onto an epoll reactor thread; a fixed worker pool executes handlers only. Long-lived connections no longer occupy a worker while idle, fixing the 0.5.x session-starvation risk and the one-shot latency regression at the source.
- `ServerOptions::max_persistent_sessions` is removed (no longer needed); `max_concurrent_streams` moved into `ServerOptions` (`0` means automatic, using `worker_threads - 1` when multiple workers exist and `1` for a single-worker server).
- Exact routes keep `on()`, and new longest-prefix routing is available via `on_prefix()` / `on_stream_prefix()`.

### Public API (breaking)

- `Request` gains `peer` (`PeerCredentials` populated from `SO_PEERCRED` on Linux / `getpeereid` on BSD, or `present == false`) and `request_id`.
- `Response.status` replaces `status_code`; a small named status set (`status_ok`, `status_not_found`, `status_request_timeout`, ...) is provided.
- Stream handlers now receive `(const StreamReader&, const Request&)`.
- `Session::request()` is genuinely concurrent (multiplexed); `request_stream()` uses an independent dedicated connection.
- `include_handler_error_messages` retained with the same default.

### Performance (measured on WSL2, i7-1260P, g++ -O3)

- One-shot `request()` p50: 74 µs (0.5.1) → **56–68 µs** (reactor removes the previous worker-teardown regression).
- Session `request()` p50 (one in-flight): **38 µs** — the reactor initially cost thread hops (64 µs), recovered by a worker-leased continuation fast path plus a client spin-wait, landing at the WSL raw round-trip floor (~34 µs; lockstep 0.5.x was 33 µs).
- Session throughput at 8-way concurrency: **~67k req/s** (vs ~48k one-shot). Streams unchanged (~5.7 GiB/s upload, ~9.5 GiB/s download).

### Server: worker-lease continuation fast path

- After the last active fixed response, its worker may lease the connection for one follow-up request during `ServerOptions::session_idle_grace` (default `1 ms`), avoiding a reactor dispatch hop. It returns the connection to the reactor before running that follow-up handler, so later pipelined requests can still execute concurrently. An idle lease returns after the grace, and `0` disables this fast path.
- One-shot requests (reserved request id `0`) return directly to the reactor after their response; only persistent-session requests use the idle-grace continuation path.
- Client sessions spin on an atomic completion flag (bounded, then a condition-variable fallback) to avoid a futex round trip per response.
- Completed session requests are removed from the in-flight table, request timeouts make the session permanently unusable, and request-id wrap avoids ids that are still active.
- Worker leases preserve partially received follow-up headers across the idle grace, stream leases unregister from epoll before hand-off, and successful streams can be followed by another request on the same wire connection. Epoll registrations carry generation tokens so stale events for a closed fd cannot affect a newer connection that reused the same descriptor number.

### Reliability and limits

- Connection descriptors stay owned until every dispatched response job releases them, preventing fd-number reuse from redirecting a late handler response. Closing connections with in-flight work continue to count against `max_connections` until that work finishes.
- Reactor inactivity checks no longer expire connections merely because a regular or serialized handler is still producing a response; stream deadlines use `stream_timeout` independently of regular `request_timeout`.
- Downloaded stream responses are cumulatively checked against the client's `max_stream_size`.
- Failed `run()` setup no longer reports the server as running, missing-route responses cannot block the reactor, and shutdown only unlinks the exact socket device/inode created by this server.

## 0.5.1

- Added `ServerOptions::max_persistent_sessions` so persistent sessions cannot starve regular RPC traffic. Every connection waiting for its next request occupies one worker, so the automatic default (`worker_threads - 1`, at least 1) reserves a worker for one-shot requests, mirroring the stream admission limit. A connection whose follow-up wait would exceed the limit is closed after its response, so the client's next request fails explicitly instead of blocking indefinitely.
- Added `ServerOptions::include_handler_error_messages` (default `true`). When disabled, handler exception messages and response-rejection reasons are replaced by a generic `Internal Server Error` body so clients cannot learn internal details.
- `Session` is now movable (move constructor/assignment); a moved-from session rejects requests with `std::logic_error`.
- Updated `docs/PROTOCOL.md` to specify the v0.5.0 persistent-connection lifecycle (lockstep multi-exchange connections, session-end conditions, serialized-route and session-limit termination).
- Added `easy_uds_session_fuzz`, a stateful libFuzzer target that feeds adversarial byte sequences into a live server over real connections, covering the session loop, read-ahead buffering, and stream frame parsing; CI runs a bounded smoke test.
- Added regression tests for session saturation (regular RPC stays available while sessions hold the reservation), error-message opt-out, session move, and moved-from rejection.
- `SECURITY.md` supported release line updated to 0.5.x.

## 0.5.0

- Added `Client::session()` returning a persistent `Session` connection whose `request()`/`request_stream()` calls reuse one socket, avoiding per-request connect/accept and teardown. On measured WSL2 hardware the tiny-RPC p50 latency dropped from ~62 µs (one-shot) to ~33 µs on a persistent session. A `Session` serializes concurrent calls, is permanently broken after any I/O error or peer close, and ends after a request to a serialized route.
- Added `Server::enqueue_maintenance()`: runs a task on the same FIFO executor as `on_serialized` handlers, strictly ordered against them, so external threads can safely mutate server-side state (for example a per-driver instance map) that serialized handlers touch. Tasks are executed exactly once, throwing tasks are contained, and `std::logic_error` is raised when the server is not running.
- Handler exceptions now propagate their `what()` message in the `500` response body (bounded by `max_message_size`), including streamed handlers and invalid handler responses, instead of a fixed `Internal Server Error`. This exposes the root cause (for example `rpc: titan instance not found`) to clients for debugging.
- Server connections now serve multiple requests (a session loop), enabling persistent connections end to end. In exchange, the one-connection-per-request path pays a small worker-teardown cost; a follow-up event-loop redesign is planned to recover it.
- Read-ahead buffering on the server/client read path coalesces frame-header and payload reads into a single `recv()`, reducing syscalls for small messages; large bodies still read directly and keep the previous syscall cost.
- Added `easy_uds_session_benchmark` and regression tests for persistent sessions, `enqueue_maintenance`, and exception-message propagation.

## 0.4.2

- Added `Server::on_serialized()` for exclusive RPC handlers that must not execute concurrently. All serialized routes share one FIFO executor, making the API suitable for robot drivers and other single-owner hardware resources accessed by multiple processes.
- Serialized requests are handed off from the regular worker pool to a dedicated executor, so commands waiting behind a long-running hardware operation do not consume normal RPC workers; regular `on()` routes such as status/health checks remain available.
- Server `request_timeout` now also bounds serialized-queue waiting time. A serialized request whose absolute deadline expires before execution is discarded without invoking its handler, preventing stale robot commands from running later.
- `Server::stop()` discards serialized requests that are still queued. A serialized handler that has already started follows the existing handler rule and cannot be forcibly cancelled by portable C++.
- Fixed a concurrent-shutdown descriptor race in the serialized handoff: the queue now owns each queued connection descriptor, and the handing-off worker transfers ownership while holding the serialized-queue lock. `stop()` can no longer close a descriptor that a handing-off worker still wraps, which previously could close or write to an unrelated reused descriptor.
- The serialized executor thread is now created lazily on the first serialized request instead of unconditionally at `run()`. Servers that never handle a serialized route run exactly `worker_threads` threads rather than `worker_threads + 1`; the FIFO executor still starts before the first serialized request arrives, including serialized routes registered after `run()` begins.
- Added regression tests for cross-route serialization, FIFO waiting, regular-RPC availability during queued hardware commands, queue-deadline expiry, and shutdown cleanup.
- No wire-protocol changes were required; existing clients use the same regular request/response frames.

## 0.4.1

- Added `Server::set_max_concurrent_streams()`. The automatic default reserves one worker for regular RPC traffic when multiple workers are configured, preventing long-lived streams from exhausting the entire worker pool without changing the `ServerOptions` ABI.
- The explicit stream limit accepts values from `1` through `worker_threads`; `0` is invalid rather than automatic or unlimited.
- Excess stream connections are rejected immediately instead of occupying the reserved RPC worker, with regression coverage for stream saturation and RPC availability.
- On Linux, socket creation and acceptance now set non-blocking and close-on-exec flags atomically, avoiding redundant `fcntl()` calls while preserving the portable fallback.
- The accept loop now drains up to 64 queued connections per readiness event, reducing polling during bursts while bounding stop/wakeup latency.
- Added a concurrent tiny-RPC benchmark reporting throughput and average/p50/p95/p99 latency.
- Preserved binary compatibility with v0.4.0: no public symbols were removed and public type sizes and alignments are unchanged.

## 0.4.0

- Added pull-based `Server::on_stream()` and `Client::request_stream()` APIs for bodies that do not fit in memory.
- Added framed request/response chunks with explicit end markers while retaining protocol version 1 compatibility for the original request/response frame types.
- Added configurable `stream_chunk_size`, `max_stream_size`, and `stream_timeout` limits. A zero stream-size/deadline limit supports long-lived producers while inactivity timeout and socket backpressure remain active.
- Added bounded receive buffering, cumulative byte-limit checks, unread-request draining, and invalid chunk-sequence rejection.
- Added multi-megabyte upload/download coverage using intentionally different producer, wire, and consumer chunk sizes.
- Removed readiness polling from the successful non-blocking I/O path and combined frame headers with payloads using `sendmsg()`, substantially reducing system calls for high-throughput streams.
- Added an optional release-mode streaming throughput benchmark.

## 0.3.0

- Reworked socket I/O around non-blocking descriptors plus `poll()` instead of relying on per-syscall socket timeouts.
- Added absolute server/client `request_timeout` deadlines to stop slow-drip peers from extending a request indefinitely.
- Added a wakeup pipe so `Server::stop()` no longer closes the listening descriptor from another thread while `run()` may still be polling it.
- Added a per-socket advisory instance lock (`<socket_path>.lock`) to serialize easy-uds startup and stale-socket cleanup.
- Added current-user ownership checks, inode revalidation, and a configurable stale-socket grace period before unlinking a refused socket.
- Added concurrent-stop/start stress coverage, slow-drip deadline coverage, client transaction-deadline coverage, and duplicate-server ownership tests.
- Factored protocol header validation into a shared internal codec and added an optional Clang/libFuzzer target.
- Changed pre-1.0 shared-library `SOVERSION` to major.minor so incompatible minor ABIs do not masquerade as the same ABI.
- Protocol version remains `1`; v0.3.0 is wire-compatible with v0.2.x.

## 0.2.0

- Replaced newline-delimited framing with a versioned binary protocol.
- Added binary-safe routes and bodies.
- Replaced detached per-connection threads with a fixed worker pool.
- Added configurable connection limits and server/client timeouts.
- Added configurable Unix socket permissions with a secure `0600` default.
- Added `Server::is_running()` and socket path accessors.
- Changed socket failures to preserve `errno` through `std::system_error`.
- Hardened stale-socket cleanup and owned-path unlinking against replacement races.
- Fixed descriptor lifetime ordering during concurrent shutdown.
- Added malformed-peer, binary payload, timeout, shutdown, stale-path, permission, and concurrency tests.
- Added static/shared CI, ASan/UBSan/TSan CI, CMake package-consumer CI, and CMake presets.
- Made tests/examples default to off when embedded as a subproject.

## 0.1.0

- Initial C++17 request/response API over Unix Domain Sockets.
- Route handlers, 1 MiB framing limit, basic concurrent handling, tests, examples, and CMake package export.
