# easy-uds

[한국어 README](README.ko.md)

`easy-uds` is a small C++17 request/response and chunk-streaming library for local IPC over Unix Domain Sockets (`AF_UNIX`). It keeps the public API intentionally small while providing bounded concurrency, deadlines, binary-safe framing, deterministic shutdown, and CMake package support.

> **Protocol note:** v0.6.0 uses protocol version 2 with a 20-byte header and request-id multiplexing. It is not wire-compatible with protocol v1 used by v0.5.x and earlier.

## Features

- C++17 with no third-party runtime dependencies
- Named request handlers with arbitrary binary request/response bodies
- Optional one-descriptor request passing with `SCM_RIGHTS` (`Client::request_fd()` → `Request::fd`)
- Multiplexed persistent sessions: concurrent `request()` calls on one connection, correlated by request id and answered in any order
- Peer credentials (`pid`/`uid`/`gid`) via Linux `SO_PEERCRED`
- Exact and longest-prefix route registration (`on()` / `on_prefix()`)
- Copy-on-write immutable handler snapshots: request dispatch takes no global handler-table mutex and does not copy `std::function`
- FIFO serialized request handlers for exclusive hardware/resources, without occupying the normal worker pool while waiting
- Incremental, constant-memory upload/download streams with configurable chunk sizes and total limits
- `Server::enqueue_maintenance()` for safe server-side state cleanup from external threads
- Natural flow control through Unix-socket backpressure
- Versioned, binary-safe protocol framing (protocol v2 with request-id multiplexing)
- epoll reactor server: idle connections never occupy a worker, and stalled fixed-response I/O never blocks the reactor or worker pool
- Configurable connection limit, inactivity timeout, absolute request deadline (`408` on expiry), connect timeout, backlog, and message size
- Optimistic non-blocking socket I/O that calls `poll()` only on backpressure
- Gathered header+payload writes through `sendmsg()` to reduce per-chunk system calls
- Owner-only socket permissions (`0600`) by default, configurable when group access is needed
- Per-socket instance lock to serialize startup/stale cleanup between easy-uds servers
- Grace period before removing a connection-refused socket pathname as stale
- Thread-safe `stop()` using a single Linux `eventfd` wakeup counter
- Handler exceptions converted to `500` with the exception message in the body (opt-out via `include_handler_error_messages`)
- Small semantic `ErrorCode` classes with the original socket `errno` preserved by `Error::system_code()`
- Static or shared library builds through `BUILD_SHARED_LIBS`
- CMake install/export package and downstream `find_package()` support
- Standalone Linux experiments for io_uring, FD passing, zero-copy, and memfd/eventfd shared-memory transport
- Unit, shutdown-race stress, ASan/UBSan/TSan, protocol/session fuzz, static/shared, and install-consumer CI coverage

## Platform

The current implementation requires Linux (`epoll` and `SO_PEERCRED`). Windows, macOS, and BSD are not currently supported. The source uses pathname sockets rather than Linux-only abstract sockets.

## Quick start

### Server

```cpp
#include <easy_uds/easy_uds.hpp>

#include <iostream>

int main() {
    easy_uds::Server server("/tmp/easy-uds.sock");

    server.on("ping", [](const easy_uds::Request&) {
        return easy_uds::Response{200, "pong"};
    });

    server.on("echo", [](const easy_uds::Request& request) {
        return easy_uds::Response{200, request.body};
    });

    std::cout << "Server listening on " << server.socket_path() << '\n';
    server.run();
}
```

### Client

```cpp
#include <easy_uds/easy_uds.hpp>

#include <iostream>

int main() {
    easy_uds::Client client("/tmp/easy-uds.sock");

    const auto response = client.request("echo", "hello");
    std::cout << response.status << ' ' << response.body << '\n';
}
```

A `Client` instance can safely be used for concurrent `request()` calls because each request owns its own socket and does not mutate client state.

### Exclusive hardware or robot commands

Use `Server::on_serialized()` for commands that must never overlap. Every serialized route shares one FIFO executor, so commands from multiple processes wait their turn while normal `on()` routes remain available on the regular worker pool.

```cpp
easy_uds::Server server("/tmp/robot-driver.sock");

server.on_serialized("drive", [](const easy_uds::Request& request) {
    // Only one serialized handler can run at a time, even if another process
    // calls a different serialized route such as "arm" concurrently.
    robot_drive(request.body);
    return easy_uds::Response{200, "ok"};
});

server.on_serialized("arm", [](const easy_uds::Request& request) {
    robot_arm(request.body);
    return easy_uds::Response{200, "ok"};
});

server.on("status", [](const easy_uds::Request&) {
    // Regular RPCs do not wait behind the serialized command queue.
    return easy_uds::Response{200, read_robot_status()};
});

server.run();
```

The FIFO order is the order in which complete serialized requests are enqueued by the server. Queue time counts toward the server's existing `request_timeout`. If that deadline expires before a queued command begins execution, the command is answered with `408` and its handler is **not** called. `stop()` also discards commands that are still waiting in the serialized queue. A serialized handler that has already started has the same cooperative-cancellation limitation as a regular handler and cannot be forcibly interrupted by portable C++.

`Server::enqueue_maintenance()` runs a task on the same FIFO executor, strictly ordered with serialized handlers, so server-side state that serialized handlers touch (for example the driver instance map) can be cleaned up safely from any thread when a client disappears:

```cpp
// Called from a sweeper thread when a client is detected dead.
server.enqueue_maintenance([&] { drivers.erase(dead_driver_name); });
```

Tasks run exactly once, in FIFO order with serialized commands, and a throwing task is caught so the executor keeps running. `enqueue_maintenance()` throws `std::logic_error` when the server is not running.

### Large or continuous bodies

`request_stream()` pulls request bytes into a reusable fixed-size buffer and delivers response bytes incrementally. Returning `0` from a `StreamReader` ends that side of the stream.

```cpp
// Server: process an upload without retaining its complete body.
server.on_stream("upload", [](const easy_uds::StreamReader& body, const easy_uds::Request&) {
    std::ofstream output("received.bin", std::ios::binary);
    std::array<char, 64 * 1024> buffer{};
    while (const std::size_t size = body(buffer.data(), buffer.size())) {
        output.write(buffer.data(), static_cast<std::streamsize>(size));
    }
    return easy_uds::StreamResponse{201, {}};
});

// Client: stream a file and consume response chunks as they arrive.
std::ifstream input("large.bin", std::ios::binary);
easy_uds::StreamReader upload = [&input](char* buffer, std::size_t capacity) {
    input.read(buffer, static_cast<std::streamsize>(capacity));
    return static_cast<std::size_t>(input.gcount());
};
const int status = client.request_stream("upload", upload, [](std::string_view chunk) {
    // Consume or persist this view before the callback returns.
});
```

The exchange is half-duplex: the request stream finishes before the response stream starts. Socket backpressure bounds in-flight data. Set `max_stream_size = 0` and `stream_timeout = 0` for an unbounded long-lived stream; `io_timeout` still closes a peer that stops making progress.

### Persistent sessions for high-frequency polling

`Client::request()` opens, uses, and closes one connection per call. For high-frequency polling (IMU, encoders, health checks), `Client::session()` opens a persistent connection whose `request()` calls reuse the socket, avoiding per-request connect/accept and teardown:

```cpp
easy_uds::Client client("/tmp/robot-driver.sock");
easy_uds::Session session = client.session();  // one connection, reused
while (true) {
    const auto response = session.request("imu", "poll");
    // ...process...
}
```

A `Session` multiplexes concurrent `request()` calls: each is correlated by a request id and answered in any order, so polling threads can share one connection without a client-side request lock. It is permanently broken after any I/O error, request timeout, or peer close — open a fresh session to reconnect. A request to a serialized route uses the exclusive executor but does not close the session. `request_stream()` uses its own dedicated connection, so stream calls do not block fixed requests or one another.

## Configuration

### Server options

```cpp
easy_uds::ServerOptions options;
options.worker_threads = 4;
options.max_connections = 64;
options.max_message_size = 1024 * 1024;
options.stream_chunk_size = 64 * 1024;
options.max_stream_size = 1024 * 1024 * 1024;
options.io_timeout = std::chrono::seconds(5);
options.request_timeout = std::chrono::seconds(30);
options.stream_timeout = std::chrono::milliseconds(0);
options.stale_socket_grace_period = std::chrono::milliseconds(250);
options.listen_backlog = 64;
options.socket_permissions = 0600;

easy_uds::Server server("/tmp/easy-uds.sock", options);
// Optional override; auto mode reserves one RPC worker.
options.max_concurrent_streams = 3;
```

| Option | Default | Meaning |
| --- | ---: | --- |
| `worker_threads` | `4` | Worker threads executing handlers |
| `max_connections` | `64` | Maximum concurrently open client connections |
| `max_message_size` | `1 MiB` | Maximum request route+body size and maximum response body size |
| `stream_chunk_size` | `64 KiB` | Reusable buffer and outgoing frame size for streamed bodies |
| `max_stream_size` | `1 GiB` | Maximum bytes per streamed request body and response body; `0` is unbounded |
| `max_total_inflight_bytes` | `0` | Strict aggregate declared request-byte budget including partial parser buffers, queued requests, and executing requests; `0` disables it |
| `max_total_output_bytes` | `0` | Aggregate unsent fixed-response wire-byte budget (header + body); `0` disables the global limit |
| `max_inflight_requests_per_connection` | `64` | Per-connection queued/executing fixed-request count high-water mark |
| `max_inflight_request_bytes_per_connection` | `4 MiB` | Per-connection queued/executing route+body byte high-water mark |
| `max_output_bytes_per_connection` | `4 MiB` | Per-connection unsent fixed-response wire-byte limit |
| `max_concurrent_streams` | `0` (auto) | Maximum simultaneous streams; auto uses `worker_threads - 1`, or `1` when only one worker exists. Explicit values must be between `1` and `worker_threads` |
| `io_timeout` | `5000 ms` | Maximum idle time between successful socket-I/O progress events; `0` disables it |
| `request_timeout` | `30000 ms` | Absolute deadline per regular request; a request that expires before a worker runs it is answered `408`. `0` disables it |
| `stream_timeout` | `0` | Absolute streaming-exchange deadline after the stream header; `0` disables it |
| `session_idle_grace` | `1 ms` | The last completing worker waits directly for one follow-up request during this grace, avoiding its reactor dispatch hop. It returns the connection to the reactor before running the handler, preserving multiplexing. `0` disables the fast path |
| `include_handler_error_messages` | `true` | Include handler exception messages in `500` bodies; disable to hide internal details from clients |
| `stale_socket_grace_period` | `250 ms` | Time to keep probing a connection-refused existing socket before treating it as stale |
| `listen_backlog` | `64` | Backlog passed to `listen()` |
| `socket_permissions` | `0600` | Filesystem permissions applied to the socket pathname |

When the connection limit is reached, newly accepted connections are closed instead of creating more workers or growing an unbounded queue.

Fixed RPC input is bounded per connection and can also be bounded across the whole server. When `max_total_inflight_bytes` is nonzero, a validated frame reserves its declared route+body bytes before parser buffers are allocated; partial, queued, and executing requests share one strict logical-byte budget. The reactor pauses only that peer's `EPOLLIN` when admission fails and resumes waiting peers below the low-water marks. The opt-in strict mode routes Session continuation reads back through the reactor so they cannot bypass admission; the default `0` keeps the 0.6.4 fast path unchanged. Bytes left in the kernel remain under Unix-socket backpressure. Fixed responses use `max_output_bytes_per_connection` to isolate slow peers and have the analogous `max_total_output_bytes` aggregate budget.

Fixed responses use a nonblocking worker fast path. A response that does not fit immediately is handed to a per-connection `EPOLLOUT` queue, so a client that stops reading cannot occupy a worker. The queued remainder is capped at the larger of 4 MiB or one maximum-size response; a peer that exceeds the cap is closed without affecting other connections. Streaming exchanges retain their exclusive worker lease and are governed by `max_concurrent_streams`.

Each stream occupies one worker until its response body is complete. The automatic stream limit is `worker_threads - 1`, or `1` for a single-worker server. This prevents long-lived streams from starving regular RPC traffic. An excess stream is closed before its body is read, so the client receives an `Error` classified as `closed`; retry it with a fresh/rewound `StreamReader`. Set `ServerOptions::max_concurrent_streams = worker_threads` to allow every worker to run a stream, or use separate server instances when short RPCs and many long-lived streams have different capacity requirements.

`request_timeout` includes time spent waiting in the worker queue, time spent waiting in the serialized-command queue, and socket I/O time. A serialized request that expires before its handler starts is discarded without executing it. User handler execution cannot be forcibly cancelled by portable C++; if a handler runs past the deadline, response I/O fails immediately after that handler returns.

### Client options

```cpp
easy_uds::ClientOptions options;
options.max_message_size = 1024 * 1024;
options.stream_chunk_size = 64 * 1024;
options.max_stream_size = 1024 * 1024 * 1024;
options.connect_timeout = std::chrono::seconds(2);
options.io_timeout = std::chrono::seconds(5);
options.request_timeout = std::chrono::seconds(30);
options.stream_timeout = std::chrono::milliseconds(0);

easy_uds::Client client("/tmp/easy-uds.sock", options);
```

`connect_timeout` bounds only connection establishment. `io_timeout` bounds inactivity between successful I/O progress. An idle `Session` does not consume this timeout: its reader waits indefinitely for the first response byte, a partial response frame uses `io_timeout`, and each pending caller is still bounded by `request_timeout`. `request_timeout` bounds a regular transaction, while `stream_timeout` bounds a streaming transaction. A value of `0` disables the corresponding limit.

Operational failures are reported as `easy_uds::Error`, which remains derived
from `std::system_error`. `kind()` (or inherited `code()`) reports a stable
easy-uds classification while `system_code()` preserves the original `errno`.

## Socket ownership and stale cleanup

Each server pathname uses a companion lock file at `<socket_path>.lock`. The lock is advisory and is held for the active server lifetime. It prevents two easy-uds servers from simultaneously deciding that the same socket is stale. The lock file is intentionally left on disk after shutdown; the kernel lock, not file existence, represents ownership.

Before removing an existing connection-refused socket, easy-uds verifies that it is a Unix socket owned by the current effective user, waits for `stale_socket_grace_period`, rechecks inode identity, and only then unlinks it. A regular file or another user's Unix socket is never intentionally removed.

Applications should place sockets in a directory whose permissions match their trust boundary, for example a private runtime directory rather than a world-writable location when practical.

## Concurrency and shutdown

`Server::run()` starts the epoll reactor and fixed worker pool, then blocks until shutdown. The serialized executor starts lazily when first needed. `run()` is intended to be called once per `Server` object. `Server::stop()` is idempotent and may be called concurrently from other threads.

During shutdown:

1. `running` is cleared and a non-blocking `eventfd` counter interrupts `epoll_wait()`;
2. the owned socket pathname is removed only if its device/inode still match;
3. every accepted client socket is `shutdown()` so blocked I/O exits;
4. the regular and serialized executors are signaled to stop, discarding work that has not started;
5. the reactor and executor threads exit and are joined;
6. connection, listener, wakeup, epoll, and instance-lock descriptors are closed.

The listener is not closed by another thread while the reactor may still be polling it, eliminating descriptor-number reuse races in the accept loop.

Handlers registered with `on()` run concurrently on worker threads. The same registered function object may be invoked by several workers at once, so mutable captures and other shared state must provide their own synchronization. Handler-table updates are copy-on-write and become visible atomically without invalidating in-flight requests. Handlers registered with `on_serialized()` instead share one dedicated FIFO executor across all serialized routes; queued serialized requests therefore do not occupy regular workers, allowing routes such as health/status RPCs to remain responsive while a hardware command is in progress.

## Wire protocol

The wire format is binary and versioned. Each message starts with a 20-byte header containing the `EUDS` magic, protocol version, message type, request id, and two 32-bit network-byte-order fields. Routes and bodies are length-delimited, so embedded NULs and newlines are preserved.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the exact layout.

Fixed requests may be pipelined on a persistent connection and are correlated by request id. A stream is half-duplex and exclusive on its wire connection; after its response ends, that connection may carry another request.

## Error behavior

- Unknown route: `404 / Not Found`
- Handler throws: `500` with the exception's `what()` as the response body (bounded by `max_message_size`; a non-`std::exception` throw yields the generic `Internal Server Error` body). Set `include_handler_error_messages = false` to always use the generic body
- Handler returns a negative status or oversized body: `500` with the rejection reason as the response body (likewise gated by `include_handler_error_messages`)
- A request that waits past its server-side `request_timeout` before a worker executes it: `408` (handler not invoked)
- Malformed/timed-out/disconnected peer: that connection is closed; the server continues running
- Connection/request deadline exceeded: `ErrorCode::timeout`, with `ETIMEDOUT` in `system_code()` on the side observing the timeout
- Invalid local arguments/configuration: `std::invalid_argument` or `std::length_error`
- Socket/OS failures: `easy_uds::Error` (also catchable as `std::system_error`)
- Invalid response framing: `ErrorCode::protocol`; response exceeding a configured receive limit: `ErrorCode::too_large`
- A broken Session rejects later requests with `ErrorCode::closed`; easy-uds does not implicitly reconnect or replay requests
- Invalid server lifecycle operation, such as a second `run()`: `std::logic_error`
- A second easy-uds `Server` claiming the same path while the first owns it: `ErrorCode::busy`, with `EADDRINUSE` in `system_code()`
- Protocol version 1 peers are rejected (v2 server)

## Build and test

Requirements:

- CMake 3.20+
- A C++17 compiler
- Linux
- POSIX threads

Using the developer preset:

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Or manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Build options:

- `EASY_UDS_BUILD_EXAMPLES=ON|OFF` — defaults to `ON` only when this is the top-level project
- `EASY_UDS_BUILD_TESTS=ON|OFF` — defaults to `ON` only when this is the top-level project
- `EASY_UDS_BUILD_FUZZERS=ON|OFF` — default `OFF`; requires upstream Clang/libFuzzer
- `EASY_UDS_BUILD_BENCHMARKS=ON|OFF` — default `OFF`
- `EASY_UDS_WARNINGS_AS_ERRORS=ON|OFF` — default `OFF`
- `BUILD_SHARED_LIBS=ON|OFF` — choose shared or static library output

When `easy-uds` is included with `add_subdirectory()`, examples and tests stay disabled by default.

### Protocol fuzz target

With upstream Clang:

```bash
CC=clang CXX=clang++ cmake -S . -B build-fuzz \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_BUILD_FUZZERS=ON
cmake --build build-fuzz --target easy_uds_protocol_fuzz
mkdir -p fuzz-corpus
./build-fuzz/easy_uds_protocol_fuzz fuzz-corpus -runs=20000 -max_len=64
```

### Throughput benchmarks

Build the benchmarks in release mode:

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
# Streaming: payload MiB, chunk bytes
./build-bench/easy_uds_stream_benchmark 1024 65536
# Tiny RPC: total requests, client concurrency (one-shot, one connection per request)
./build-bench/easy_uds_rpc_benchmark 100000 8
# Tiny RPC over persistent sessions: total requests, independent sessions
./build-bench/easy_uds_session_benchmark 200000 8
# Tiny RPC multiplexing: total requests, concurrent callers on one shared session
./build-bench/easy_uds_session_benchmark 200000 8 shared
# Warmed-up tiny session RPC: total ordinary heap allocations
./build-bench/easy_uds_allocation_benchmark 20000
```

The streaming benchmark generates bytes on demand and discards them at the receiver, so it measures the library and local socket path without disk-I/O effects. The RPC benchmarks measure client-side latency on the one-connection-per-request API and on the persistent `Client::session()` API respectively, reporting aggregate throughput plus average, p50, p95, p99, p99.9, and maximum request latency. The session benchmark gives each caller its own session by default; pass `shared` to measure request-id multiplexing and client-side contention on one session.

For the reproducible 0.6.x closing gate, enable experiment targets and run the
architecture-neutral benchmark plus repeated test soak:

```bash
./scripts/final_linux_benchmarks.sh build-bench
./scripts/long_soak.sh build-bench 20
```

The `workflow_dispatch` CI path runs the same matrix on native Ubuntu x86_64
and hosted ARM64, then uploads both complete logs.

The session spin window is a build-time tuning knob for latency experiments; the default is `100` microseconds. Build benchmark variants with `-DEASY_UDS_SESSION_SPIN_US=0`, `10`, `25`, `50`, or `100` and compare p50/p95/p99, throughput, CPU, and context switches. The session benchmark reports user/system CPU time and voluntary/involuntary context switches through `getrusage()`. It is intentionally not a public runtime option until measurements show a stable policy. On hosts with `perf` or `strace`, wrap the same benchmark to collect syscall/request, cache-miss, and branch-miss counters.

Reference numbers (WSL2 on an i7-1260P, g++ 15, `-O3`):

```text
one-shot request()    p50 ~56–68 µs            ~13k req/s @ c1
session request()     p50 ~38 µs (1 in-flight) ~67k req/s @ c8 independent sessions
shared Session        p50 ~0.5 ms              ~25k req/s @ c16
tiny session RPC      ~0 ordinary heap allocations/request after warm-up
stream (64 KiB chunks)  upload ~5.7 GiB/s, download ~9.5 GiB/s
```

The reactor makes one-shot latency independent of connection teardown. Sessions recover the near-floor latency with a worker-lease continuation fast path (the last completing worker waits for one follow-up request, then returns the connection to the reactor before executing its handler), plus a client-side spin-wait. This keeps the low-latency sequential path while allowing later requests to run concurrently during a slow handler.

## Run the examples

Start the server:

```bash
./build/easy_uds_server_example
```

Then run the client from another terminal:

```bash
./build/easy_uds_client_example
```

## Install and use from CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF
cmake --build build --parallel
cmake --install build --prefix /path/to/prefix
```

Downstream project:

```cmake
find_package(easy_uds CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE easy_uds::easy_uds)
```

Configure the downstream project with the install prefix when necessary:

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/prefix
```

For pre-1.0 shared builds, the ELF `SOVERSION` tracks the major and minor release (for example `0.4`) because minor releases may change ABI before 1.0.

## Public API

`#include <easy_uds/easy_uds.hpp>` remains the complete beginner-friendly
umbrella. Feature headers such as `client.hpp`, `session.hpp`, and `server.hpp`
are also self-contained; see the [public header map](docs/api/headers.md).

### `easy_uds::Server`

- `Server(std::string socket_path, ServerOptions options = {})`
- `on(std::string route, Handler handler)`
- `on_prefix(std::string prefix, Handler handler)`
- `on_serialized(std::string route, Handler handler)`
- `enqueue_maintenance(std::function<void()> task)`
- `on_stream(std::string route, StreamHandler handler)`
- `on_stream_prefix(std::string prefix, StreamHandler handler)`
- `run()`
- `stop()`
- `is_running()`
- `socket_path()`

### `easy_uds::Client`

- `Client(std::string socket_path, ClientOptions options = {})`
- `request(std::string_view route, std::string_view body = {})`
- `request_fd(std::string_view route, BorrowedFd fd, std::string_view body = {})` — sends a duplicate of a caller-owned descriptor via `SCM_RIGHTS`
- `request_stream(std::string_view route, const StreamReader&, response_chunk)`
- `session()`
- `socket_path()`

### `easy_uds::Session`

- `status()` — lock-free `active` / `broken` / `moved_from` snapshot
- `valid()` — true exactly while `status() == SessionStatus::active`
- `request(std::string_view route, std::string_view body = {})` — multiplexed, concurrent-safe
- `request_stream(std::string_view route, const StreamReader&, response_chunk)` — independent dedicated connection

### Data types

```cpp
using Status = std::int32_t;  // status_ok=200, status_request_timeout=408, status_not_found=404, ...

enum class ErrorCode {
    system, timeout, closed, protocol, busy, too_large,
    invalid_request, unavailable, cancelled
};

enum class SessionStatus { active, broken, moved_from };

struct PeerCredentials {
    pid_t pid; uid_t uid; gid_t gid;
    bool present;  // false when the platform cannot provide credentials
};

struct Request {
    std::string route;
    std::string body;
    PeerCredentials peer;
    std::uint32_t request_id;
    OwnedFd fd;  // received descriptor; closes automatically with Request
};

struct Response {
    Status status = 200;
    std::string body;
};

using StreamReader = std::function<std::size_t(char*, std::size_t)>;

struct StreamResponse {
    Status status = 200;
…
};
```

FD ownership and retention semantics are documented in
[`docs/api/fd-passing.md`](docs/api/fd-passing.md). Source changes from 0.6 are
listed in [`docs/migration/0.6-to-0.7.md`](docs/migration/0.6-to-0.7.md).
Error classification and preserved OS details are documented in
[`docs/api/errors.md`](docs/api/errors.md). Session state, retry, and
concurrency semantics are documented in
[`docs/api/session.md`](docs/api/session.md).

## Security scope

`easy-uds` is local IPC, not a network security protocol. It provides no application-level authentication, authorization, encryption, or sandboxing. The default socket pathname mode is `0600`; choose the containing directory and permissions according to your trust boundary.

The instance lock coordinates easy-uds servers using the same path. It cannot force unrelated software that ignores the lock file to participate in that protocol.

## Repository layout

```text
include/easy_uds/       Umbrella plus feature-specific public headers
src/client/             One-shot Client, persistent Session, shared transport
src/server/             Server lifecycle, routing, startup, and shutdown
src/reactor/            Reactor core, parser, dispatch, flow control, output,
                        streaming, and worker executors
src/protocol/           Protocol-v2 codec boundary
src/detail/             Descriptor, deadline, socket, and exact-I/O utilities
examples/               Minimal server/client examples
tests/easy_uds_test/     Unit tests grouped by subsystem
tests/                  Stress, fuzz, benchmark, and package-consumer tests
cmake/                  Installed-package CMake config
docs/                   Protocol documentation
docs/ROADMAP_0.6.md     0.6.x technical experiment and release boundaries
docs/ROADMAP_0.7.md     0.7 usability, API, and compatibility plan
docs/PERF_0.7.md        0.7 regression measurements against v0.6.4
docs/EXPERIMENTS_0.6.md  Standalone UDS capability probes
docs/PERF_0.6.md         0.6 benchmark measurements and interpretation
.github/workflows/      GitHub Actions CI
```

## License

MIT. See [`LICENSE`](LICENSE).
