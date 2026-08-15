# easy-uds 0.7.0 release record

This document records the final 0.7.0 scope, verification gates, and accepted
tradeoffs. The public API is frozen for the 0.7.x maintenance line.

## Scope freeze

0.7 keeps protocol v2, explicit ownership, cooperative deadlines, no implicit
retry/replay, named serialization domains, queue policies, bounded stats
snapshots, and the 0.6.4 hot path. It adds only the narrow fixed-RPC
`easy_uds::simple` facade as a Core adapter. The RC does not add protocol v3,
auto reconnect, priority scheduling, full-duplex streaming, multiple-FD
frames, a new transport, or an embedded exporter/logger.

## Beginner path

- [Simple API getting started](simple-api/getting-started.md) reaches fixed
  `/echo` with only `Server`, `on`, `run`, `Client`, and `request`; the Core
  [Getting Started](getting-started/README.md) path remains available for
  explicit `Request`/`Response` control.
- [`examples/server.cpp`](../examples/server.cpp) and
  [`examples/client.cpp`](../examples/client.cpp) are fixed-RPC only.
- Streaming has a separate [example pair](examples/streaming.md); Robot HAL is
  an advanced showcase with explicit resource mutexes and domains.
- [ERGONOMICS_0.7.md](ERGONOMICS_0.7.md) records the syntax measurements and
  the rejected `RouteOptions` alternatives.

## Verification gates

- The local one-command gate is `bash scripts/release_gate.sh`. It configures
  and builds both static and shared Release/Werror trees, runs the complete
  CTest suite plus `ctest -L rc`, compiles the invalid-usage probes, installs
  each package variant, and runs both package consumers (including the real
  beginner `/echo` process smoke).
- Release/Werror unit and stress tests, plus the dedicated
  `easy_uds.rc_adversarial` test.
- ASan/UBSan, TSan, protocol/session fuzz, and compile-error UX probes with
  GCC/Clang.
- Static/shared install consumers, including the real beginner package
  consumer in [`tests/beginner`](../tests/beginner), plus the promoted Simple
  API consumer in [`tests/simple_consumer`](../tests/simple_consumer).
- Simple/Core c1/c8/c32 A/B and allocation probes are recorded in
  [`experiments/simple_api/DESIGN.md`](../experiments/simple_api/DESIGN.md).
- The same five-run alternating probe on native x86_64 and ARM64 did not
  reproduce the WSL2 c32 delta; the result is recorded as scheduler-sensitive
  rather than treated as a public Simple regression.
- Native x86_64 and ARM64 final benchmark/soak workflows.
- Markdown link and example build checks.
- The [public API freeze audit](api/public-api-audit.md) has no unresolved
  ownership, lifetime, moved-from, thread-safety, or exception-contract item.

`scripts/long_soak.sh` repeats the complete CTest suite for a caller-selected
number of passes. Set `EASY_UDS_SOAK_BENCHMARKS=1` to add one-shot, shared
Session, and streaming workloads to every pass; their benchmark output
includes p50/p99/throughput and `/usr/bin/time` records RSS and context
switches. Hosted workflows additionally run the native x86_64 and ARM64
benchmark/soak jobs; their logs are artifacts rather than portable
performance promises. This makes hour-scale, six-hour, or overnight runs a
parameter choice without changing the test binary.

The exact performance comparison and current stabilization status are kept in
[PERF_0.7.md](PERF_0.7.md) and [ROADMAP_0.7.md](ROADMAP_0.7.md).

## Known tradeoffs accepted for 0.7

- `simple::ResponseError` separates application non-200 responses from Core
  transport errors; aggregate `Response` construction remains the escape hatch
  for explicit statuses.
- The Simple route proxy is temporary-only, and only `()`/
  `(std::string_view)` handlers with string-like results are supported.
- `RouteOptions{handler}.serialize_in(...)` is explicit and slightly verbose,
  but avoids a template-heavy handler/options API and preserves compile-error
  locality.
- Stats snapshots are best-effort and non-transactional; cumulative counters
  remain opt-in.
- Session and handler cancellation remain cooperative. A deadline does not
  interrupt a blocking hardware operation.

## Release status

The final release uses annotated tag `v0.7.0` and the title
`easy-uds v0.7.0 — Again Easily`. Passing this checklist does not authorize
future feature additions to the 0.7.x line; only bug fixes, documentation, and
small compatibility corrections are in scope.
