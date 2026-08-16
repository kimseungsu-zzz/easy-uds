# Getting started

For the shortest fixed-RPC path, start with the installed
[Simple API guide](../simple-api/getting-started.md). It covers constant and
string-view handlers while keeping the full Core API available as an explicit
escape hatch.

This is the shortest path from an empty Linux checkout to a working request
and response. The examples use URI-shaped route names (for example, `/echo`)
consistently; route names are opaque strings to easy-uds, so this convention is
for readability and tooling rather than a wire-level requirement.

## 1. Configure and build

easy-uds targets Linux and Windows 10+ and requires C++17 and CMake 3.20 or
newer. The selected platform backend supplies the native socket/runtime
primitives:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_EXAMPLES=ON \
  -DEASY_UDS_BUILD_TESTS=ON
cmake --build build --parallel
```

## 2. Run the basic example

In one terminal:

```bash
./build/easy_uds_server_example
```

In another terminal:

```bash
./build/easy_uds_client_example
```

The server registers only `/ping` and `/echo`; the first run is fixed RPC with
no streaming, Session, or scheduling concepts. The pathname socket is removed
by the server's normal lifecycle cleanup; choose a different path as the first
argument when running your own server.

## 3. The smallest application

The beginner API has one server entry point and one client entry point:

```cpp
#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/my-service.sock");
    server.on("/echo", [](const easy_uds::Request& request) {
        return easy_uds::Response::ok(request.body);
    });
    server.run();
}
```

```cpp
easy_uds::Client client("/tmp/my-service.sock");
const auto response = client.request("/echo", "hello");
```

`Client::request()` opens and closes one connection. For polling or concurrent
callers, create one persistent `Client::session()` and share that move-only
`Session` explicitly; see [Session](../api/session.md).

`Response::ok()` is a convenience for the common successful reply. The
aggregate form, `Response{status, body}`, remains available when an explicit
status is needed. Status values follow familiar numeric conventions such as
`200`, `404`, and `409`, but easy-uds is not HTTP and does not implement HTTP
headers, methods, or routing rules.

## 4. Choose the next example

| Need | Start here | What it teaches |
|---|---|---|
| Basic fixed RPC | [`examples/server.cpp`](../../examples/server.cpp) | plain routes and fixed response |
| Streaming when needed | [`examples/streaming_server.cpp`](../../examples/streaming_server.cpp) + [`streaming_client.cpp`](../../examples/streaming_client.cpp) | pull callbacks, chunk lifetime, half-duplex stream |
| Persistent polling | [Session API](../api/session.md) | multiplexing, state, timeout behavior |
| Ordered hardware commands | [Robot HAL walkthrough](../examples/robot-hal.md) | ownership boundaries, domains, policies, context, stats |
| Descriptor transfer | [FD passing](../api/fd-passing.md) | borrowed input and owned request lifetime |
| Operational visibility | [Production diagnostics](../guides/production-diagnostics.md) | bounded snapshots and app-owned probes |

Keep the plain `on()`/`request()` path until you have a concrete requirement for
Sessions, streams, serialized domains, or diagnostics. Advanced features are
opt-in and do not change protocol v2 framing.
