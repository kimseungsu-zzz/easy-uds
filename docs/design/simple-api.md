# Simple API design experiment

This document records the promotion audit for the beginner facade. The
implementation was first measured under `experiments/simple_api/`; the narrow
v1 adapter now lives in `include/easy_uds/simple.hpp` without destabilizing the
frozen Core API. The experiment directory retains the full decision record.

## Goal

Keep the existing Core API exactly as it is while making the first fixed RPC
readable to someone who knows basic C++:

```cpp
#include <easy_uds/simple.hpp>

easy_uds::simple::Server server("/tmp/app.sock");
server.on("/ping") = "pong";
server.on("/echo") = [](std::string_view body) {
    return std::string(body);
};
server.run();
```

The intended levels remain separate:

| Level | Surface |
|---|---|
| Simple | `simple::Server`, `on(route) = value/handler`, `simple::Client::request()` |
| Core | `easy_uds::Server`, `Request`, `Response`, `Session`, options |
| Advanced | `RequestContext`, domains/policies, streams, FD ownership, stats, budgets |

Simple is an adapter, not a second engine. It must reuse Core route
registration, duplicate-route rejection, shutdown, errors, framing, and worker
execution. Protocol v2 and the reactor are out of scope.

## Promotion decisions

- Header location: `include/easy_uds/simple.hpp`; the experiment shim remains
  under `experiments/simple_api/simple.hpp` for reproducible prototype builds.
- Namespace: `easy_uds::simple`; `simple` makes the boundary visible and leaves
  the existing `easy_uds::Server` names untouched.
- `on(route)` returns a non-copyable, short-lived assignment proxy. Assignment
  registers immediately through Core. An unassigned proxy has no effect.
- Supported input signatures: `()` and `(std::string_view)` only.
- Supported successful results: `std::string`, `std::string_view`, and
  `const char*`; all become `Response::ok(...)`.
- Custom statuses, `Request`, context, streams, FD passing, queue policy,
  stats, and memory controls stay in Core.
- The promoted facade exposes an explicit `core()` escape hatch so advanced
  routes share the same server/client lifecycle and registry. It is a
  reference to the underlying Core object, not a second engine or helper
  registration path.
- Non-200 application responses throw `simple::ResponseError`, preserving the
  status and body. Transport, protocol, timeout, and closed-connection
  failures continue to use `easy_uds::Error`; status is never discarded.

## Proxy/lifetime audit

The following cases are required before promotion:

1. `server.on("/ping") = "pong"` registers exactly once.
2. Re-registering the same route follows Core's explicit rejection semantics.
3. `auto route = server.on("/ping")` is non-copyable and does not outlive its
   owning server.
4. An unassigned route proxy does not create a route.
5. Simple and Core registrations can coexist on one server.
6. `stop()` remains idempotent and a broken client is reported through the
   documented error boundary.

## Compile-error UX

The facade must reject unsupported forms at its public boundary with a short
diagnostic. In particular, these are invalid:

```cpp
server.on("/x") = 1234;
server.on("/x") = [](int value) { return value; };
server.on("/x") = [](std::string_view) { return 1234; };
```

The implementation must not expose a wall of `std::function` or
`std::invoke_result` internals as the primary message. GCC and Clang probes
belong beside the prototype and must be kept separate from the production
Core compile-error suite.

## Historical promotion gate

The production facade was promoted after all of the following were
demonstrated:

- static/shared package consumer can use the installed facade without
  changing Core behavior;
- server/client lifecycle, duplicate routes, empty bodies, string input/output,
  capturing lambdas, and Core coexistence pass on GCC and Clang;
- invalid handlers fail with understandable diagnostics;
- Simple vs Core benchmarks report no material regression in p50, p99,
  throughput, CPU-s/request, or warm allocations;
- no new protocol, router, type-erasure registry, retry, reconnect, or hidden
  allocation is needed;
- the error/status policy is explicit and documented;
- the API remains small enough that `simple.hpp` does not become a second
  advanced framework.

The gate is met for the narrow v1 surface and Simple v1 is frozen. Typed RPC
(`server.on("/add") = add`, codecs, function traits) remains a separate future
experiment and is not part of this adapter. Changes to the frozen surface
require a new design note, migration note, and fresh Core/Simple performance
gate.
