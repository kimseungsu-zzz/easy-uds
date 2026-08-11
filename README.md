# easy-uds

[한국어 README](README.ko.md)

`easy-uds` is a small C++17 request/response and chunk-streaming library for local IPC over Unix Domain Sockets (`AF_UNIX`). It keeps the public API intentionally small while providing bounded concurrency, deadlines, binary-safe framing, deterministic shutdown, and CMake package support.

> **Protocol note:** v0.6.0 uses protocol version 2 with a 20-byte header and request-id multiplexing. It is not wire-compatible with protocol v1 used by v0.5.x and earlier.

## Features

- C++17 with no third-party runtime dependencies
- Named request handlers with arbitrary binary request/response bodies
- Multiplexed persistent sessions: concurrent `request()` calls on one connection, correlated by request id and answered in any order
- Peer credentials (`pid`/`uid`/`gid`) via Linux `SO_PEERCRED`
- Exact and longest-prefix route registration (`on()` / `on_prefix()`)
- FIFO serialized request handlers for exclusive hardware/resources, without occupying the normal worker pool while waiting
- Incremental, constant-memory upload/download streams with configurable chunk sizes and total limits
- `Server::enqueue_maintenance()` for safe server-side state cleanup from external threads
- Natural flow control through Unix-socket backpressure
- Versioned, binary-safe protocol framing (protocol v2 with request-id multiplexing)
- epoll reactor server: idle connections never occupy a worker
- Configurable connection limit, inactivity timeout, absolute request deadline (`408` on expiry), connect timeout, backlog, and message size
- Optimistic non-blocking socket I/O that calls `poll()` only on backpressure
- Gathered header+payload writes through `sendmsg()` to reduce per-chunk system calls
- Owner-only socket permissions (`0600`) by default, configurable when group access is needed
- Per-socket instance lock to serialize startup/stale cleanup between easy-uds servers
- Grace period before removing a connection-refused socket pathname as stale
- Thread-safe `stop()` using a wakeup pipe
- Handler exceptions converted to `500` with the exception message in the body (opt-out via `include_handler_error_messages`)
- `std::system_error` for socket failures, preserving the underlying `errno`
- Static or shared library builds through `BUILD_SHARED_LIBS`
- CMake install/export package and downstream `find_package()` support
- Unit, shutdown-race stress, ASan/UBSan/TSan, protocol/session fuzz, static/shared, and install-consumer CI coverage

## Platform

The 0.6 implementation requires Linux (`epoll` and `SO_PEERCRED`). Windows, macOS, and BSD are not currently supported. The source uses pathname sockets rather than Linux-only abstract sockets.

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

The FIFO order is the order in which complete serialized requests are handed off by the regular workers. Queue time counts toward the server's existing `request_timeout`. If that deadline expires before a queued command begins execution, the command is answered with `408` and its handler is **not** called. `stop()` also discards commands that are still waiting in the serialized queue. A serialized handler that has already started has the same cooperative-cancellation limitation as a regular handler and cannot be forcibly interrupted by portable C++.

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
| `max_concurrent_streams` | `0` (auto) | Maximum simultaneous streams; auto reserves one worker (`worker_threads - 1`) for regular RPC. Explicit values must be between `1` and `worker_threads` |
| `io_timeout` | `5000 ms` | Maximum idle time between successful socket-I/O progress events; `0` disables it |
| `request_timeout` | `30000 ms` | Absolute deadline per request; a request that expires before a worker runs it is answered `408`. `0` disables it |
| `stream_timeout` | `0` | Absolute streaming-exchange deadline after the stream header; `0` disables it |
| `session_idle_grace` | `1 ms` | A worker that just served a fixed request keeps reading the connection directly (no reactor hop per request) while the peer keeps sending within this grace; after an idle gap it returns the connection to the reactor. `0` disables the fast path |
| `include_handler_error_messages` | `true` | Include handler exception messages in `500` bodies; disable to hide internal details from clients |
| `stale_socket_grace_period` | `250 ms` | Time to keep probing a connection-refused existing socket before treating it as stale |
| `listen_backlog` | `64` | Backlog passed to `listen()` |
| `socket_permissions` | `0600` | Filesystem permissions applied to the socket pathname |

When the connection limit is reached, newly accepted connections are closed instead of creating more workers or growing an unbounded queue.

Each stream occupies one worker until its response body is complete. The automatic stream limit is `worker_threads - 1`, or `1` for a single-worker server. This prevents long-lived streams from starving regular RPC traffic. An excess stream is closed before its body is read, so the client receives a `std::system_error`; retry it with a fresh/rewound `StreamReader`. Set `ServerOptions::max_concurrent_streams = worker_threads` to allow every worker to run a stream, or use separate server instances when short RPCs and many long-lived streams have different capacity requirements.

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

`connect_timeout` bounds only connection establishment. `io_timeout` bounds inactivity between successful I/O progress. `request_timeout` bounds a regular transaction, while `stream_timeout` bounds a streaming transaction. A value of `0` disables the corresponding limit.

Socket failures and timeouts are reported as `std::system_error`; timeouts use `ETIMEDOUT`.

## Socket ownership and stale cleanup

Each server pathname uses a companion lock file at `<socket_path>.lock`. The lock is advisory and is held for the active server lifetime. It prevents two easy-uds servers from simultaneously deciding that the same socket is stale. The lock file is intentionally left on disk after shutdown; the kernel lock, not file existence, represents ownership.

Before removing an existing connection-refused socket, easy-uds verifies that it is a Unix socket owned by the current effective user, waits for `stale_socket_grace_period`, rechecks inode identity, and only then unlinks it. A regular file or another user's Unix socket is never intentionally removed.

Applications should place sockets in a directory whose permissions match their trust boundary, for example a private runtime directory rather than a world-writable location when practical.

## Concurrency and shutdown

`Server::run()` starts a fixed worker pool and blocks in a `poll()`-based accept loop. It is intended to run once per `Server` object. `Server::stop()` is idempotent and may be called concurrently from other threads.

During shutdown:

1. `running` is cleared and a non-blocking wakeup pipe interrupts the accept-loop `poll()`;
2. the owned socket pathname is removed only if its device/inode still match;
3. queued clients are closed;
4. active client sockets are `shutdown()` so blocked/polled I/O exits;
5. `run()` joins the worker pool;
6. the run thread closes the listener/wakeup descriptors and releases the instance lock.

The listener is not closed by another thread while `run()` may still be polling it, eliminating descriptor-number reuse races in the accept loop.

Handlers registered with `on()` run concurrently on worker threads. If they share mutable state, that state must provide its own synchronization. Handlers registered with `on_serialized()` instead share one dedicated FIFO executor across all serialized routes; queued serialized requests therefore do not occupy regular workers, allowing routes such as health/status RPCs to remain responsive while a hardware command is in progress.

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
- Connection/request deadline exceeded: `std::system_error` with `ETIMEDOUT` on the side observing the timeout
- Invalid local arguments/configuration: `std::invalid_argument` or `std::length_error`
- Socket/OS failures: `std::system_error`
- Invalid server lifecycle operation, such as a second `run()`: `std::logic_error`
- A second easy-uds `Server` claiming the same path while the first owns it: `std::system_error` with `EADDRINUSE`
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
# Tiny RPC over persistent sessions: total requests, concurrent sessions
./build-bench/easy_uds_session_benchmark 200000 8
```

The streaming benchmark generates bytes on demand and discards them at the receiver, so it measures the library and local socket path without disk-I/O effects. The RPC benchmarks measure client-side latency on the one-connection-per-request API and on the persistent `Client::session()` API respectively, reporting aggregate throughput plus average, p50, p95, and p99 request latency.

Reference numbers (WSL2 on an i7-1260P, g++ 15, `-O3`):

```text
one-shot request()    p50 ~56–68 µs            ~13k req/s @ c1
session request()     p50 ~38 µs (1 in-flight) ~67k req/s @ c8
stream (64 KiB chunks)  upload ~5.7 GiB/s, download ~9.5 GiB/s
```

The reactor makes one-shot latency independent of connection teardown. Sessions recover the near-floor latency with a worker-lease continuation fast path (the serving worker keeps reading the connection until the peer pauses longer than `session_idle_grace`, then returns it to the reactor), plus a client-side spin-wait — so a high-frequency poller on a session reaches the same p50 as 0.5.x lockstep while the reactor still absorbs idle connections.

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
- `request_stream(std::string_view route, const StreamReader&, response_chunk)`
- `session()`
- `socket_path()`

### `easy_uds::Session`

- `request(std::string_view route, std::string_view body = {})` — multiplexed, concurrent-safe
- `request_stream(std::string_view route, const StreamReader&, response_chunk)` — independent dedicated connection

### Data types

```cpp
using Status = std::int32_t;  // status_ok=200, status_request_timeout=408, status_not_found=404, ...

struct PeerCredentials {
    pid_t pid; uid_t uid; gid_t gid;
    bool present;  // false when the platform cannot provide credentials
};

struct Request {
    std::string route;
    std::string body;
    PeerCredentials peer;
    std::uint32_t request_id;
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

## Security scope

`easy-uds` is local IPC, not a network security protocol. It provides no application-level authentication, authorization, encryption, or sandboxing. The default socket pathname mode is `0600`; choose the containing directory and permissions according to your trust boundary.

The instance lock coordinates easy-uds servers using the same path. It cannot force unrelated software that ignores the lock file to participate in that protocol.

## Repository layout

```text
include/easy_uds/       Public headers
src/                    Library implementation and internal protocol codec
examples/               Minimal server/client examples
tests/                  Unit, stress, fuzz, and package-consumer tests
cmake/                  Installed-package CMake config
docs/                   Protocol documentation
.github/workflows/      GitHub Actions CI
```

## License

MIT. See [`LICENSE`](LICENSE).
