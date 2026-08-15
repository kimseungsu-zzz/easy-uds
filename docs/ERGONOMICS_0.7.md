# 0.7 beginner ergonomics audit

This audit treats a new user as someone who knows basic C++ but has not read
the reactor, protocol, or 0.6 performance history. The first successful fixed
RPC must require only `Server`, `on()`, `run()`, `Client`, `request()`, and a
basic response value.

## Fixed-RPC first-success measurement

Counting convention: blank lines and comments are excluded; the snippet starts
at the include and ends at the closing brace. A type is counted once even when
it appears in both server and client snippets.

```cpp
#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Server server("/tmp/demo.sock");
    server.on("/echo", [](const easy_uds::Request& request) {
        return easy_uds::Response::ok(request.body);
    });
    server.run();
}
```

```cpp
#include <easy_uds/easy_uds.hpp>

int main() {
    easy_uds::Client client("/tmp/demo.sock");
    auto result = client.request("/echo", "hello");
}
```

| Revision/candidate | Server lines | Client lines | easy-uds types | Mandatory qualified symbols | Handler signature types | Calls to first response |
|---|---:|---:|---:|---:|---:|---:|
| 0.6.4 aggregate form | 8 | 5 | 4 (`Server`, `Request`, `Response`, `Client`) | 5 (adds `status_ok`) | 1 | 3 (`on`, `run`, `request`) |
| 0.7 before helper | 8 | 5 | 4 | 5 (adds `status_ok`) | 1 | 3 |
| 0.7 `Response::ok()` candidate | 8 | 5 | 4 | 4 | 1 | 3 |

The helper does not remove the explicit `Request` model or hide status handling;
it removes only the common `status_ok` spelling. The aggregate form remains
available for `404`, `409`, custom non-negative statuses, and code that wants
the wire value visible at the return site.

## Response candidates

| Candidate | Decision | Reason |
|---|---|---|
| `Response{status_ok, body}` | Keep | Zero magic, aggregate initialization, explicit status, no ABI/layout change |
| `Response::ok(body)` | Adopt | Reads naturally for the first success path, inline/layout-neutral, keeps aggregate form |
| `Response::error(code, body)` | Reject for 0.7 | “Error” is application policy, not a second response hierarchy; status constants already make the intended value explicit |

Status numbers are familiar conventions, not HTTP. easy-uds has no HTTP method,
header, or router semantics; route names are opaque strings and `/` is a
readability convention.

## RouteOptions candidates

The three pre-release prototypes were compared on reading order,
handler/metadata separation, overload count, C++17 diagnostics, callable
copy/move behavior, ABI impact, and future extension:

```cpp
// A. Current wrapper form
server.on("/drive/velocity",
          easy_uds::RouteOptions{handler}
              .serialize_in("drivetrain", easy_uds::QueuePolicy::latest_wins));

// B. Handler/options separation
easy_uds::RouteOptions options;
options.serialize_in("drivetrain", easy_uds::QueuePolicy::latest_wins);
server.on("/drive/velocity", handler, options);

// C. Factory/builder form
server.on("/drive/velocity", handler,
          easy_uds::RouteOptions::serialized(
              "drivetrain", easy_uds::QueuePolicy::latest_wins));
```

| Candidate | Result |
|---|---|
| A | **Retained.** One advanced overload, no new callable deduction, scheduling metadata stays attached to the route registration, and `std::function` storage is moved once. |
| B | Rejected. Adds an overload family and allows a handler/options mismatch that produces less local diagnostics; copying a populated `RouteOptions` is also an easy accidental cost. |
| C | Rejected. The factory hides that `RouteOptions` owns the handler and would require a second builder representation or a more template-heavy callable path. |

The current form is documented as an advanced route registration, not as the
beginner `on()` path. No template signature inference or automatic retry was
added.

## Progressive disclosure

| Level | Public concepts |
|---|---|
| Beginner | `Server`, `on`, `run`, `Client`, `request`, `Response::ok`, aggregate `Response` |
| Normal | `Session`, timeout options, `on_serialized`, basic `ServerOptions` |
| Advanced | `RequestContext`, named domains, `LatestWins`, `RejectIfBusy`, streams, FD ownership, stats, strict budgets, peer credentials |

The README and [Getting Started](getting-started/README.md) stay on the first
level until the first fixed response succeeds. The [streaming example](examples/streaming.md)
and [Robot HAL showcase](examples/robot-hal.md) are deliberately linked as
next-step material rather than included in the first program.

## Compile-error UX

`scripts/compile_error_smoke.sh` compiles six intentionally invalid public API
snippets with GCC (and the CI matrix repeats them with Clang): wrong handler
signature, wrong return type, invalid queue-policy argument, wrong stream
signature, copying `OwnedFd`, and copying `RequestContext`. The test requires
each compiler to fail with a non-empty diagnostic instead of silently accepting
an unsafe conversion. Runtime-only contracts such as empty serialization
domains and moved-from object use remain normal unit-test cases because C++
cannot diagnose them statically without hiding the explicit API model.

## Simple facade promotion boundary

The optional fixed-RPC facade is now promoted separately as
`<easy_uds/simple.hpp>` under `easy_uds::simple`. It does not replace the Core
examples above: it is a string-body adapter with a temporary-only route proxy.
Application non-200 responses use `simple::ResponseError`, while transport and
protocol failures remain `easy_uds::Error`. Streaming, sessions, queue policy,
FD ownership, stats, and strict budgets intentionally stay Core concepts.
