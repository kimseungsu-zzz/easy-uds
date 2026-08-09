# easy-uds

`easy-uds` is a small C++17 request/response library for local IPC over Unix Domain Sockets (`AF_UNIX`). It keeps the public API intentionally small while providing bounded concurrency, absolute request deadlines, binary-safe framing, deterministic shutdown, and CMake package support.

> **Protocol note:** v0.2.0 introduced protocol version 1. v0.3.0 remains wire-compatible with v0.2.x; v0.1.x is not wire-compatible.

## Features

- C++17 with no third-party runtime dependencies
- Named request handlers with arbitrary binary request/response bodies
- Versioned, binary-safe protocol framing
- Fixed worker pool instead of one detached thread per connection
- Configurable connection limit, inactivity timeout, absolute request deadline, connect timeout, backlog, and message size
- Non-blocking socket I/O driven by `poll()`, avoiding indefinite blocking inside `send()`/`recv()`
- Owner-only socket permissions (`0600`) by default, configurable when group access is needed
- Per-socket instance lock to serialize startup/stale cleanup between easy-uds servers
- Grace period before removing a connection-refused socket pathname as stale
- Thread-safe `stop()` using a wakeup pipe; the stopping thread never closes the listener while `run()` may still poll it
- Handler exceptions converted to `500 / Internal Server Error`
- `std::system_error` for socket failures, preserving the underlying `errno`
- Static or shared library builds through `BUILD_SHARED_LIBS`
- CMake install/export package and downstream `find_package()` support
- Unit, shutdown-race stress, ASan/UBSan/TSan, protocol fuzz, static/shared, and install-consumer CI coverage

## Platform

The implementation uses POSIX Unix-domain socket APIs and is tested on Linux. Windows is not supported. The source uses pathname sockets rather than Linux-only abstract sockets.

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
    std::cout << response.status_code << ' ' << response.body << '\n';
}
```

A `Client` instance can safely be used for concurrent `request()` calls because each request owns its own socket and does not mutate client state.

## Configuration

### Server options

```cpp
easy_uds::ServerOptions options;
options.worker_threads = 4;
options.max_connections = 64;
options.max_message_size = 1024 * 1024;
options.io_timeout = std::chrono::seconds(5);
options.request_timeout = std::chrono::seconds(30);
options.stale_socket_grace_period = std::chrono::milliseconds(250);
options.listen_backlog = 64;
options.socket_permissions = 0600;

easy_uds::Server server("/tmp/easy-uds.sock", options);
```

| Option | Default | Meaning |
| --- | ---: | --- |
| `worker_threads` | `4` | Number of worker threads processing accepted clients |
| `max_connections` | `64` | Maximum active + queued client connections |
| `max_message_size` | `1 MiB` | Maximum request route+body size and maximum response body size |
| `io_timeout` | `5000 ms` | Maximum idle time between successful socket-I/O progress events; `0` disables it |
| `request_timeout` | `30000 ms` | Absolute deadline from accept until response I/O completes; `0` disables it |
| `stale_socket_grace_period` | `250 ms` | Time to keep probing a connection-refused existing socket before treating it as stale |
| `listen_backlog` | `64` | Backlog passed to `listen()` |
| `socket_permissions` | `0600` | Filesystem permissions applied to the socket pathname |

When the connection limit is reached, newly accepted connections are closed instead of creating more workers or growing an unbounded queue.

`request_timeout` includes time spent waiting in the worker queue and socket I/O time. User handler execution cannot be forcibly cancelled by portable C++; if a handler runs past the deadline, response I/O fails immediately after that handler returns.

### Client options

```cpp
easy_uds::ClientOptions options;
options.max_message_size = 1024 * 1024;
options.connect_timeout = std::chrono::seconds(2);
options.io_timeout = std::chrono::seconds(5);
options.request_timeout = std::chrono::seconds(30);

easy_uds::Client client("/tmp/easy-uds.sock", options);
```

`connect_timeout` bounds only connection establishment. `io_timeout` bounds inactivity between successful I/O progress. `request_timeout` is an absolute deadline for the entire `connect + request write + response read` transaction. A value of `0` disables the corresponding limit.

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

User handlers run concurrently on worker threads. If handlers share mutable state, that state must provide its own synchronization.

## Wire protocol

The wire format is binary and versioned. Each message starts with a 16-byte header containing the `EUDS` magic, protocol version, message type, and two 32-bit network-byte-order fields. Routes and bodies are length-delimited, so embedded NULs and newlines are preserved.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the exact layout.

Each connection carries exactly one request and one response.

## Error behavior

- Unknown route: `404 / Not Found`
- Handler throws: `500 / Internal Server Error`
- Handler returns a negative status or oversized body: `500 / Internal Server Error`
- Malformed/timed-out/disconnected peer: that connection is closed; the server continues running
- Connection/request deadline exceeded: `std::system_error` with `ETIMEDOUT` on the side observing the timeout
- Invalid local arguments/configuration: `std::invalid_argument` or `std::length_error`
- Socket/OS failures: `std::system_error`
- Invalid server lifecycle operation, such as a second `run()`: `std::logic_error`
- A second easy-uds `Server` claiming the same path while the first owns it: `std::system_error` with `EADDRINUSE`

## Build and test

Requirements:

- CMake 3.20+
- A C++17 compiler
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

For pre-1.0 shared builds, the ELF `SOVERSION` tracks the major and minor release (for example `0.3`) because minor releases may change ABI before 1.0.

## Public API

### `easy_uds::Server`

- `Server(std::string socket_path, ServerOptions options = {})`
- `on(std::string route, Handler handler)`
- `run()`
- `stop()`
- `is_running()`
- `socket_path()`

### `easy_uds::Client`

- `Client(std::string socket_path, ClientOptions options = {})`
- `request(std::string_view route, std::string_view body = {})`
- `socket_path()`

### Data types

```cpp
struct Request {
    std::string route;
    std::string body;
};

struct Response {
    int status_code = 200;
    std::string body;
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
