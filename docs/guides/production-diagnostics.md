# Production diagnostics

easy-uds deliberately stops at bounded state snapshots. It does not start a
metrics exporter, logging backend, background sampler, or tracing pipeline on
behalf of an application.

## What the library provides

- `Server::stats()` reports active connections, in-flight work, worker and
  serialized queue depth, retained input/output bytes, and (when enabled)
  cumulative dispatch/rejection counters.
- `Session::stats()` reports the fixed-request multiplexer's observed in-flight
  depth and cumulative session events when the client option enables them.
- Snapshots are thread-safe and best-effort. Individual fields may describe
  different instants; do not treat one snapshot as a transaction.
- Gauge fields remain available with `StatsMode::disabled`. Cumulative counters
  are opt-in so the default request path avoids counter RMW operations.

## Recommended application boundary

Expose two small application-owned probes rather than making every caller
understand the library's internal counters:

```cpp
#include <string>

server.on("/health", [](const easy_uds::Request&) {
    return easy_uds::Response{easy_uds::status_ok, "ok"};
});

server.on("/diagnostics", [&server](const easy_uds::Request&) {
    const auto snapshot = server.stats();
    return easy_uds::Response{
        easy_uds::status_ok,
        "active_connections=" +
            std::to_string(snapshot.active_connections)};
});
```

Keep `/health` short, stable, and cheap enough for a watchdog or readiness
probe. Put evolving operator detail in `/diagnostics` or translate the same
fields into the application's existing monitoring pipeline. The complete
resource-aware composition is shown in the [Robot HAL walkthrough](../examples/robot-hal.md).

## Sampling and privacy

Sample diagnostics at the monitoring layer; do not poll once per request. A
diagnostics response can include peer or route information only when the
application's access policy permits it. On Linux, read peer metadata through
`posix::request_capabilities(context).peer_credentials()`; it is local
credential metadata, not an authorization decision. Keep `include_handler_error_messages`
disabled when response bodies must not expose internal details.

## What is intentionally out of scope

The library does not promise a stable exporter schema, log format, histogram
implementation, or distributed trace context in 1.0. Low-level tracing remains
a build-time diagnostic option. New observability features belong after 1.0 and
must first preserve
the no-overhead default and have a measured use case.
