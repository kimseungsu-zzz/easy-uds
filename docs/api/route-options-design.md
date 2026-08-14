# RouteOptions, serialization domains, and queue policy

`RouteOptions` is the opt-in registration path for contextual handlers and
advanced serialized scheduling. The beginner API remains unchanged:

```cpp
server.on("status", [](const easy_uds::Request&) {
    return easy_uds::Response{easy_uds::status_ok, read_status()};
});

server.on_serialized("reset", [](const easy_uds::Request&) {
    reset_robot();
    return easy_uds::Response{easy_uds::status_ok, "ok"};
});
```

Every `on_serialized()` route and `enqueue_maintenance()` task shares the
default domain. It executes exactly one item at a time in FIFO order, matching
the 0.6 behavior.

## Named domains

Independent hardware resources can use independent domains:

```cpp
server.on(
    "velocity/set",
    easy_uds::RouteOptions{
        [](const easy_uds::Request& request) {
            set_velocity(request.body);
            return easy_uds::Response{easy_uds::status_ok, "ok"};
        }}
        .serialize_in("drivetrain",
                      easy_uds::QueuePolicy::latest_wins));

server.on(
    "arm/move",
    easy_uds::RouteOptions{
        [](const easy_uds::Request& request,
           const easy_uds::RequestContext& context) {
            return move_arm(request, context);
        }}
        .serialize_in("arm"));
```

One domain executes at most one handler at a time. Different domains may run
in parallel up to `ServerOptions::max_concurrent_serialized_domains`. Zero selects
`worker_threads`; the executor starts with no threads and grows only when
independent domains actually require parallel service. It never allocates a
per-domain permanent thread.

`RouteOptions` accepts both `Handler(const Request&)` and
`Handler(const Request&, const RequestContext&)`. Scheduling does not force a
simple handler to accept an unused context. `on()` and `on_prefix()` use the
same options object, so no `on_latest_*` or `on_domain_*` overload family is
needed.

Passing a named domain or non-FIFO policy to `on_serialized()` is rejected as
ambiguous. Use `on(route, RouteOptions{...}.serialize_in(...))` for advanced
scheduling. An empty domain passed to `serialize_in()` names the existing
default domain.

## Queue policies

```cpp
enum class QueuePolicy {
    fifo,
    latest_wins,
    reject_if_busy,
};
```

- `fifo` runs every admitted request in per-domain admission order.
- `latest_wins` replaces queued work with the same concrete request route in
  the same domain. It never interrupts a handler that has begun.
- `reject_if_busy` rejects when the domain has an executing or queued item.

A superseded or busy request receives a normal protocol response with
`status_conflict` (409). The connection and Session remain usable; automatic
retry or replay is never performed. A caller therefore distinguishes an
application scheduling decision from a transport failure.

For `on_prefix()`, the concrete incoming route is the replacement key. Two
different routes matching one prefix do not replace each other. Body-derived
keys are deliberately unsupported because they add callback lifetime,
exception, and allocation costs to admission.

Server deadline expiry is still checked immediately before handler execution.
A policy never extends the deadline, and contextual handlers observe the same
deadline and cooperative-stop state through `RequestContext`.

## Observability and cost

`ServerStats::serialized_queue_depth` counts waiting requests and maintenance
tasks across every domain. `active_serialized_domains` counts domains currently
executing an item. With `StatsMode::basic`, superseded and busy outcomes are
reported separately by `serialized_requests_superseded` and
`serialized_requests_rejected_busy`.

The regular `on()` route entry, lookup, and dispatch layout are unchanged.
Domain lookup, policy branches, and lazy executor growth occur only after a
route explicitly selects serialized scheduling. The default serialized domain
also preserves one-at-a-time FIFO semantics.
