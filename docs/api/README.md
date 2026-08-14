# API reference

The public API is intentionally split by responsibility while
`<easy_uds/easy_uds.hpp>` remains the recommended umbrella include. Each page
documents the contract dimensions that matter in production: parameters,
return values, thread safety, ownership, lifetime, timeout, errors, and
performance cost.

## Reference map

| Area | Header | Reference |
|---|---|---|
| Server, routes, streams, options | [`server.hpp`](../../include/easy_uds/server.hpp) / [`options.hpp`](../../include/easy_uds/options.hpp) | [Core API](core.md), [RouteOptions and serialization](route-options-design.md) |
| One-shot client | [`client.hpp`](../../include/easy_uds/client.hpp) | [Core API](core.md), [Getting started](../getting-started/README.md) |
| Persistent multiplexed client | [`session.hpp`](../../include/easy_uds/session.hpp) | [Session state](session.md) |
| Request and response values | [`request.hpp`](../../include/easy_uds/request.hpp) / [`response.hpp`](../../include/easy_uds/response.hpp) | [FD passing](fd-passing.md), [Request context](request-context.md) |
| Streaming | [`stream.hpp`](../../include/easy_uds/stream.hpp) | [Core API](core.md) and [`examples/server.cpp`](../../examples/server.cpp) |
| Error classification | [`error.hpp`](../../include/easy_uds/error.hpp) | [Error model](errors.md) |
| Descriptor ownership | [`fd.hpp`](../../include/easy_uds/fd.hpp) | [FD passing](fd-passing.md) |
| Request metadata | [`request_context.hpp`](../../include/easy_uds/request_context.hpp) | [Request context](request-context.md) |
| Operational snapshots | [`stats.hpp`](../../include/easy_uds/stats.hpp) | [Runtime statistics](stats.md) |
| Public include/layout | [`easy_uds.hpp`](../../include/easy_uds/easy_uds.hpp) | [Headers and source layout](headers.md) |

## Contract checklist

- **Thread safety:** `Client` one-shot calls are concurrent-safe; a `Session`
  supports concurrent `request()` calls but must not be moved or destroyed while
  another operation uses the same object.
- **Ownership:** `Request::fd` owns a received descriptor; `BorrowedFd` never
  closes its input; `OwnedFd` is move-only and closes on destruction.
- **Timeout:** client/server request deadlines bound the operation and do not
  forcibly interrupt a handler. `RequestContext::stop_requested()` is
  cooperative input for interruptible work.
- **Queue policy:** plain routes run on the worker pool. `on_serialized()` is
  default-domain FIFO; `RouteOptions::serialize_in()` opts into named domains
  and `fifo`, `latest_wins`, or `reject_if_busy`.
- **Diagnostics:** `Server::stats()` and `Session::stats()` are bounded,
  best-effort snapshots. They are not transactional metrics exports and do not
  add per-request counter writes when cumulative counters are disabled.

The header comments are the normative API details; these pages explain how to
compose them without requiring a source-tree tour.
