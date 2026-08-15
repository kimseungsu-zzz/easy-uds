# API reference

The public API is intentionally split by responsibility while
`<easy_uds/easy_uds.hpp>` remains the recommended umbrella include. Each page
documents the contract dimensions that matter in production: parameters,
return values, thread safety, ownership, lifetime, timeout, errors, and
performance cost.

## Reference map

| Area | Header | Reference |
|---|---|---|
| Server, routes, streams, options | [`server.hpp`](../../src/user/cpp/core/easy_uds/server.hpp) / [`options.hpp`](../../src/user/cpp/core/easy_uds/options.hpp) | [Core API](core.md), [Option contracts](options.md), [RouteOptions and serialization](route-options-design.md) |
| One-shot client | [`client.hpp`](../../src/user/cpp/core/easy_uds/client.hpp) | [Core API](core.md), [Getting started](../getting-started/README.md) |
| Persistent multiplexed client | [`session.hpp`](../../src/user/cpp/core/easy_uds/session.hpp) | [Session state](session.md) |
| Request and response values | [`request.hpp`](../../src/user/cpp/core/easy_uds/request.hpp) / [`response.hpp`](../../src/user/cpp/core/easy_uds/response.hpp) | [FD passing](fd-passing.md), [Request context](request-context.md) |
| POSIX request capabilities | [`posix.hpp`](../../src/user/cpp/core/easy_uds/posix.hpp) / [`peer_credentials.hpp`](../../src/user/cpp/core/easy_uds/peer_credentials.hpp) | [FD passing](fd-passing.md), [Request context](request-context.md) |
| Streaming | [`stream.hpp`](../../src/user/cpp/core/easy_uds/stream.hpp) | [Core API](core.md) and [streaming example](../examples/streaming.md) |
| Error classification | [`error.hpp`](../../src/user/cpp/core/easy_uds/error.hpp) | [Error model](errors.md) |
| Descriptor ownership | [`fd.hpp`](../../src/user/cpp/core/easy_uds/fd.hpp) | [FD passing](fd-passing.md) |
| Request metadata | [`request_context.hpp`](../../src/user/cpp/core/easy_uds/request_context.hpp) | [Request context](request-context.md) |
| Operational snapshots | [`stats.hpp`](../../src/user/cpp/core/easy_uds/stats.hpp) | [Runtime statistics](stats.md) |
| Beginner fixed RPC facade | [`simple.hpp`](../../src/user/cpp/simple/easy_uds/simple.hpp) | [Simple API guide](../simple-api/getting-started.md) |
| Public include/layout | [`easy_uds.hpp`](../../src/user/cpp/core/easy_uds/easy_uds.hpp) | [Headers and source layout](headers.md) |

## Contract checklist

- **Thread safety:** `Client` one-shot calls are concurrent-safe; a `Session`
  supports concurrent `request()` calls but must not be moved or destroyed while
  another operation uses the same object.
- **Ownership:** the internal request job owns a received descriptor;
  `posix::RequestCapabilities::received_fd()` is a handler-scoped
  `BorrowedFd` view. `BorrowedFd` never closes its input; `OwnedFd` is move-only
  and closes on destruction.
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

For the beginner/advanced boundary and the final syntax comparison, see the
[0.7 ergonomics audit](../ERGONOMICS_0.7.md).

The final header-by-header freeze checklist is in the
[public API audit](public-api-audit.md).

The Request capability footprint and ownership record is in
[`../PERF_0.8_REQUEST_CAPABILITIES.md`](../PERF_0.8_REQUEST_CAPABILITIES.md).
