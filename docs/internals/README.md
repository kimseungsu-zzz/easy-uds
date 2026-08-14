# Internals

These pages explain implementation choices and benchmark evidence. They are
not required to use the public API and may change without a source-compatible
release promise.

- [Protocol v2](../PROTOCOL.md) — frame layout and wire semantics.
- [0.6 performance](../PERF_0.6.md) — baseline and accepted hot-path work.
- [0.7 performance](../PERF_0.7.md) — API-foundation A/B measurements.
- [0.6 experiments](../history/experiments/0.6.md) — adopted and rejected experiments,
  including io_uring and shared-memory probes.
- [History index](../history/README.md) — historical measurements kept out of
  the beginner API path.
- [0.7 release-candidate record](../RELEASE_0.7.md) — scope freeze and final
  verification gates.
- [Public headers and source layout](../api/headers.md) — where the stable
  boundary ends and implementation details begin.

The experiment source is intentionally not installed or included by the
library target. Build it explicitly with `-DEASY_UDS_BUILD_EXPERIMENTS=ON` when
reproducing a historical result.

When a proposed runtime feature changes protocol framing, lifetime semantics,
or the default hot path, record the design and a baseline comparison here
before adding it to the beginner documentation.
