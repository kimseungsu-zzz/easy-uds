# Internals and history

The 1.0 user-facing contract is documented in the
[compatibility contract](../api/compatibility.md). The pages below explain
implementation boundaries and retained validation evidence; they are not a
second public API.

- [Linux dependency and backend audit](linux-dependency-audit.md) — complete
  Phase 5 inventory, temporary POSIX seams, and CMake backend source selection.
- [Platform support](../platform-support.md) — validated Linux scope, Windows
  AF_UNIX scope, and intentional unsupported capabilities.
- [Windows backend notes](windows-backend.md) — concrete backend choices and
  the external CI validation boundary.
- [0.8 blocker journal](blocker-journal-0.8.md) — known portability and
  environment blockers with their non-blocking work split.

These pages explain implementation choices and benchmark evidence. They are
not required to use the public API and may change without a source-compatible
release promise.

- [Protocol v2](../PROTOCOL.md) — frame layout and wire semantics.
- [0.6 performance](../PERF_0.6.md) — baseline and accepted hot-path work.
- [0.7 performance](../PERF_0.7.md) — API-foundation A/B measurements.
- [0.8 Request capabilities](../PERF_0.8_REQUEST_CAPABILITIES.md) — Request
  footprint, ownership chain, and no-allocation policy.
- [0.8 RC performance](../PERF_0.8_RC.md) — Linux reference measurements
  and explicit Windows/ARM64 measurement boundaries.
- [0.6 experiments](../history/experiments/0.6.md) — adopted and rejected experiments,
  including io_uring and shared-memory probes.
- [History index](../history/README.md) — historical measurements kept out of
  the beginner API path.
- [0.7.0 release record](../RELEASE_0.7.md) — scope freeze and final
  verification gates.
- [0.7.1 architecture release](../releases/v0.7.1.md) — source ownership,
  backend assembly, and handoff scope.
- [0.8.0 release](../releases/v0.8.0.md) — Windows backend scope, final
  validation boundary, and deferred capabilities.
- [0.9.0 stabilization release](../releases/v0.9.0.md) — final regression
  evidence and 1.0 freeze preparation.
- [0.8.0-rc.1 historical candidate](../releases/v0.8.0-rc.1.md) — RC-era
  validation record.
- [0.9 stabilization roadmap](../ROADMAP_0.9.md) — bug-fix scope, 1.0
  freeze candidates, and deferred portability work.
- [Public headers and source layout](../api/headers.md) — where the stable
  boundary ends and implementation details begin.

The experiment source is intentionally not installed or included by the
library target. Build it explicitly with `-DEASY_UDS_BUILD_EXPERIMENTS=ON` when
reproducing a historical result.

When a proposed runtime feature changes protocol framing, lifetime semantics,
or the default hot path, record the design and a baseline comparison here
before adding it to the beginner documentation.

- [Simple API design audit](../design/simple-api.md) — assignment-style
  beginner facade promotion record and scope boundary.
- [User/system dependency audit](user-system-dependencies.md) — current public
  C++ edges and concrete separation candidates for 0.7.1.
- Phase 3 concrete seams are recorded in the dependency audit: public Client,
  Session, and route-registration glue is user-owned, while engine state and
  registration translation remain concrete system functions.
