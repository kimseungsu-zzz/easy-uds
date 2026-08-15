# Simple API getting started

The Simple API is the beginner facade for fixed request/response RPC. It is a
thin adapter over the same Core `Server`/`Client`, protocol, reactor, and
shutdown behavior. It does not add retry, reconnect, streaming, queue policy,
or serialization magic.

## Server

```cpp
#include <easy_uds/simple.hpp>

using namespace easy_uds::simple;

int main() {
    Server server("/tmp/easy-uds-simple.sock");
    server.on("/ping") = "pong";
    server.on("/echo") = [](std::string_view body) {
        return std::string(body);
    };
    server.run();
}
```

## Client

```cpp
#include <easy_uds/simple.hpp>

#include <iostream>

using namespace easy_uds::simple;

int main() {
    Client client("/tmp/easy-uds-simple.sock");
    std::cout << client.request("/ping") << '\n';
    std::cout << client.request("/echo", "hello") << '\n';
}
```

The supported Simple handler signatures are exactly `()` and
`(std::string_view)`. Successful results are `std::string`, `std::string_view`,
or `const char*`. A null C string is a deterministic handler failure.

## Errors and Core escape hatch

Transport, protocol, timeout, and closed-connection failures remain
`easy_uds::Error`. A server response with a non-200 application status throws
`easy_uds::simple::ResponseError`, which preserves both `status()` and
`body()`:

```cpp
try {
    auto body = client.request("/motor");
} catch (const easy_uds::simple::ResponseError& error) {
    std::cerr << error.status() << ": " << error.body() << '\n';
} catch (const easy_uds::Error& error) {
    // Transport/protocol/deadline failure.
}
```

For request metadata, custom statuses, streaming, FD passing, sessions,
queue policies, stats, or strict budgets, use the Core API directly:

```cpp
Server server("/tmp/easy-uds-simple.sock");
server.core().on(
    "/motor",
    easy_uds::RouteOptions{advanced_handler}
        .serialize_in("drivetrain", easy_uds::QueuePolicy::latest_wins));
```

`server.on(route)` returns a temporary-only registration proxy. The intended
form is `server.on("/ping") = "pong"`; retaining the proxy in a local variable
is rejected so its lifetime cannot be mistaken for a route handle. Duplicate
routes follow Core registration semantics and are rejected.
