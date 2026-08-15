# 0.7 public API freeze audit

This is the final source-level checklist for the 0.7 release candidate. The
audit is deliberately about contract clarity, not adding another abstraction
layer. Protocol v2, the wire header, and the 0.6.4 hot path remain unchanged.

## Header-by-header boundary

| Header | Ownership/lifetime contract | Concurrency and failure contract |
|---|---|---|
| `easy_uds.hpp` | Umbrella only; it owns no state | Compatibility include; no additional runtime cost |
| `server.hpp` | `Server` owns its reactor, workers, and listening socket until `stop()`/destruction | Registration precedes `run()`; `stop()` is idempotent and wakes blocked work |
| `client.hpp` | `Client` owns one-shot connection setup; `session()` returns a move-only `Session` | One-shot calls are concurrent-safe; failures throw `Error` |
| `session.hpp` | `Session` is move-only; moved-from objects are explicit `moved_from` | `request()` calls may be concurrent; move/destruction cannot race an active call; broken sessions do not reconnect or replay |
| `request.hpp` | `Request::fd` is an owning `OwnedFd` when present | Handler receives the request by const reference; descriptor lifetime ends with the handler/job |
| `response.hpp` | Value type; aggregate construction remains supported | `Response::ok()` is a convenience for status 200; status integers are easy-uds conventions, not HTTP |
| `stream.hpp` | `StreamReader` is a non-owning callback view for one stream invocation | Stream callbacks run under the documented deadline/backpressure contract; no hidden buffering promise |
| `options.hpp` | Options are copied/moved at registration or construction; defaults are explicit | `RouteOptions` opt-in enables context/domain/policy; no automatic retry/reconnect |
| `request_context.hpp` | Non-copyable, non-movable, callback-scoped read-only view | `stop_requested()` is cooperative and does not interrupt user code |
| `fd.hpp` | `BorrowedFd` never closes; `OwnedFd` is one-int, move-only, closes on destruction | `duplicate()` is the only operation that allocates a descriptor; invalid use reports `Error` |
| `error.hpp` | `Error` is an ordinary exception value; copied `ErrorCode`/system code remain valid | Semantic `ErrorCode` is paired with the original OS `system_code()` and remains catchable as `std::system_error` |
| `stats.hpp` | Snapshots are values; optional cumulative counters are shared internally | Snapshots are best-effort/non-transactional; disabled counters do not add per-request atomic RMWs |
| `version.hpp` | Compile-time version constants only | No runtime state or ABI ownership |

## Review questions applied

- Names expose ownership (`BorrowedFd`/`OwnedFd`), lifetime (`RequestContext`),
  and state (`SessionStatus`) instead of relying on comments alone.
- Move-only types delete copy operations explicitly. Moved-from `Session` and
  invalid `OwnedFd` behavior is documented and covered by tests.
- Observer methods and value accessors are `const`; non-throwing observers are
  `noexcept` where the implementation can guarantee it.
- Thread-safety boundaries are stated for `Client`, `Session`, `Server::stats`,
  and `RequestContext`; no API implies that a deadline forcibly kills a
  handler.
- Beginner code does not need `RouteOptions`, streams, descriptors, stats, or
  context. Those concepts are linked as progressive-disclosure next steps.

## Mechanical checks

The installed package consumer compiles every public header independently in
`tests/package_consumer/CMakeLists.txt`; the RC gate runs that consumer for
both static and shared builds. The same gate builds all examples with
`-Wall -Wextra -Wpedantic -Werror`, runs the unit/stress/RC-labelled tests, and
compiles the intentional invalid-usage probes with the selected compiler.

No source-breaking public API change is accepted after this audit without a
new migration note and a fresh performance gate. Creating `v0.7.0` remains a
separate user approval step.
