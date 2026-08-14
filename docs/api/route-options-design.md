# RouteOptions scheduling shape (Phase 3 design lock)

This document fixes the intended public shape before serialized domains and
queue policies are implemented. It is a design contract for Phase 3, not a
claim that the methods below exist in the current build.

## Beginner API remains unchanged

```cpp
server.on("status", [](const easy_uds::Request&) {
    return easy_uds::Response{200, read_status()};
});

server.on_serialized("reset", [](const easy_uds::Request&) {
    reset_robot();
    return easy_uds::Response{200, "ok"};
});
```

`on_serialized()` continues to mean the default serialization domain with FIFO
ordering. Users who do not need domains or policies never construct an option.

## Target advanced shape

The intended Phase 3 form is one registration overload and one extensible
options object:

```cpp
enum class QueuePolicy {
    fifo,
    latest_wins,
    reject_if_busy,
};

server.on(
    "velocity/set",
    easy_uds::RouteOptions{
        [](const easy_uds::Request& request) {
            set_velocity(request.body);
            return easy_uds::Response{200, "ok"};
        }}
        .serialize_in("drivetrain", easy_uds::QueuePolicy::latest_wins));
```

`RouteOptions` must accept both the existing simple handler and the contextual
handler. Scheduling must not force a handler that does not need
`RequestContext` to accept and ignore it. `on()`, `on_prefix()`, and the
existing contextual registrations then share the same option type; no new
`on_latest_*`, `on_domain_*`, or handler-arity overload family is added.

The exact builder spelling will receive a naming review with the rest of
Phase 2, but the semantic tuple is fixed as:

```text
handler + optional RequestContext + optional serialization(domain, policy)
```

## Domain and policy semantics

- An empty/default domain is the existing global serialized FIFO.
- Different named domains may execute concurrently; one domain executes at
  most one handler at a time.
- `fifo` runs every admitted request in admission order.
- `latest_wins` replaces only queued work with the same concrete request route
  in the same domain. It never interrupts a handler that has begun.
- `reject_if_busy` rejects when the domain has an executing or queued request.
- A superseded or busy request receives `status_conflict` (409) before handler
  execution. Transport success and application rejection remain distinct.
- Prefix registration uses the concrete incoming route as the replacement
  key. Custom body-derived key callbacks are deliberately outside the first
  0.7 implementation because they complicate lifetime and exception rules.
- Deadline expiry is evaluated before handler execution and remains observable
  through `RequestContext`; a policy never extends a deadline.

Stats will count queue replacement and busy rejection as separate Phase 3
counters. Queue depth remains a gauge. This makes policy behavior observable
without changing protocol v2 or adding client-side retry/replay.

## Implementation boundary

Domains should be logical serialized lanes scheduled by a bounded executor,
not one permanently allocated thread per user string. Registration validates
domain/policy combinations on the cold path. The default `on()` hot path keeps
the current handler-entry tag and pays no domain lookup or policy branch.

Phase 3 implementation is accepted only if the existing simple RPC and default
serialized FIFO remain inside the 0.6.4 regression gates.
