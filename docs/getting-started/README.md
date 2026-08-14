# Getting started

This is the shortest path from an empty Linux checkout to a working request
and response. The examples use URI-shaped route names (for example, `/echo`)
consistently; route names are opaque strings to easy-uds, so this convention is
for readability and tooling rather than a wire-level requirement.

## 1. Configure and build

easy-uds currently targets Linux and requires C++17, CMake 3.20 or newer, and
`pthread` support:

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

The server registers `/ping`, `/echo`, and `/discard`. The client demonstrates
one-shot fixed requests followed by a streaming request. The pathname socket is
removed by the server's normal lifecycle cleanup; choose a different path as
the first argument when running your own server.

## 3. The smallest application

The beginner API has one server entry point and one client entry point:

```cpp
#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/my-service.sock");
    server.on("/echo", [](const easy_uds::Request& request) {
        return easy_uds::Response{easy_uds::status_ok, request.body};
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

## 4. Choose the next example

| Need | Start here | What it teaches |
|---|---|---|
| Basic RPC and streaming | [`examples/server.cpp`](../../examples/server.cpp) | plain routes, fixed response, stream route |
| Persistent polling | [Session API](../api/session.md) | multiplexing, state, timeout behavior |
| Ordered hardware commands | [Robot HAL walkthrough](../examples/robot-hal.md) | ownership boundaries, domains, policies, context, stats |
| Descriptor transfer | [FD passing](../api/fd-passing.md) | borrowed input and owned request lifetime |
| Operational visibility | [Production diagnostics](../guides/production-diagnostics.md) | bounded snapshots and app-owned probes |

Keep the plain `on()`/`request()` path until you have a concrete requirement for
Sessions, streams, serialized domains, or diagnostics. Advanced features are
opt-in and do not change protocol v2 framing.
