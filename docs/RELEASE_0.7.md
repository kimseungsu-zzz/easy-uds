# easy-uds 0.7 release-candidate record

This document records why the public API is ready to freeze. It is a release
candidate audit, not a release announcement; creating a tag or GitHub release
still requires explicit user approval.

## Scope freeze

0.7 keeps protocol v2, explicit ownership, cooperative deadlines, no implicit
retry/replay, named serialization domains, queue policies, bounded stats
snapshots, and the 0.6.4 hot path. The RC does not add protocol v3, auto
reconnect, priority scheduling, full-duplex streaming, multiple-FD frames, a
new transport, or an embedded exporter/logger.

## Beginner path

- [Getting Started](getting-started/README.md) reaches fixed `/echo` with only
  `Server`, `on`, `run`, `Client`, `request`, and `Response::ok`.
- [`examples/server.cpp`](../examples/server.cpp) and
  [`examples/client.cpp`](../examples/client.cpp) are fixed-RPC only.
- Streaming has a separate [example pair](examples/streaming.md); Robot HAL is
  an advanced showcase with explicit resource mutexes and domains.
- [ERGONOMICS_0.7.md](ERGONOMICS_0.7.md) records the syntax measurements and
  the rejected `RouteOptions` alternatives.

## Verification gates

- Release/Werror unit and stress tests, plus the dedicated
  `easy_uds.rc_adversarial` test.
- ASan/UBSan, TSan, protocol/session fuzz, and compile-error UX probes with
  GCC/Clang.
- Static/shared install consumers, including the real beginner package
  consumer in [`tests/beginner`](../tests/beginner).
- Native x86_64 and ARM64 final benchmark/soak workflows.
- Markdown link and example build checks.

The exact performance comparison and current stabilization status are kept in
[PERF_0.7.md](PERF_0.7.md) and [ROADMAP_0.7.md](ROADMAP_0.7.md).

## Known tradeoffs accepted for 0.7

- `Response::ok()` is intentionally the only new beginner response helper;
  aggregate construction remains the escape hatch for explicit statuses.
- `RouteOptions{handler}.serialize_in(...)` is explicit and slightly verbose,
  but avoids a template-heavy handler/options API and preserves compile-error
  locality.
- Stats snapshots are best-effort and non-transactional; cumulative counters
  remain opt-in.
- Session and handler cancellation remain cooperative. A deadline does not
  interrupt a blocking hardware operation.

## Approval boundary

After the final CI workflow is green, the remaining operation is a user-facing
decision: create an annotated `v0.7.0` tag and release notes, or continue the
RC audit. No release is implied by passing this document's checklist.
