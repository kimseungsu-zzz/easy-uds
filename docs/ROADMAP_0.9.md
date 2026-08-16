# 0.9 stabilization roadmap

The `v0.8.0` tag is an immutable release baseline. The 0.9 line is a
stabilization line: it may correct observable bugs, documentation, tests, and
packaging defects, but it does not reopen the 0.8 architecture or add a new
transport/API feature.

## Scope and guardrails

- Keep protocol version 2 and the wire format unchanged.
- Keep the public Core and Simple API behavior unchanged unless a concrete
  correctness or ownership defect is demonstrated.
- Do not add typed RPC, protocol v3, C/Python bindings, generic handles,
  automatic retry/replay, Windows resource passing, SID/token identity, or an
  IOCP rewrite.
- Do not modify or force-push `v0.8.0`; fixes belong on `main` and a future
  0.9.x release.
- Preserve the build-time `user -> system -> platform` ownership boundary and
  the explicit CMake platform source sets.

## Stabilization inventory

| Area | Current contract to preserve | 0.9 evidence/check |
| --- | --- | --- |
| Request and capabilities | Move-only, platform-neutral `Request`; POSIX capabilities are callback-scoped views; internal descriptor ownership is unique | API/header audit, descriptor and peer-identity tests, ASan/UBSan/TSan |
| Errors and deadlines | Semantic `ErrorCode` with original system code; absolute deadlines and cooperative handler stop | error, timeout, session, and socket-wait tests; Windows WSA mapping smoke |
| Session | Concurrent request-id multiplexing, explicit broken/moved-from state, no retry/replay | session, shutdown, stress, fuzz, and long-soak tests |
| Server lifecycle | Idempotent stop, safe restart, Linux pathname/lock defenses, Windows AF_UNIX lifecycle | lifecycle tests and hosted Windows Core/package jobs |
| Queues and streaming | FIFO/LatestWins/RejectIfBusy semantics, bounded accounting, stream backpressure and cleanup | serialized, budget, streaming, adversarial, and stress tests |
| Simple API | Fixed beginner surface, `ResponseError`, `core()` escape hatch, no implicit policy | GCC/Clang Simple tests, diagnostics, installed consumers |
| Packaging | Static/shared CMake targets, public header tree, version metadata, imported consumer target | release gate and Windows static/shared package consumers |
| Architecture | Concrete platform capabilities selected by CMake; no runtime virtual backend or user/platform leakage | `scripts/check_architecture.sh` and architecture CI job |

The first stabilization correction widened the hosted Windows CTest timeout in
`3b56769` after a scheduler-sensitive 30-second timeout with no product
assertion. Current API documentation was then aligned with the implemented
Request/capability contract in `c0f4070`.

## 1.0 freeze-candidate inventory

Before 1.0, review each item once against the installed headers, migration
guides, package consumers, and protocol golden tests. A review is not a reason
to rename a stable API cosmetically.

1. **Public values and ownership:** `Request`, `Response`, `RequestContext`,
   `OwnedFd`, `BorrowedFd`, `PeerCredentials`, and
   `posix::RequestCapabilities`.
2. **Lifecycle and concurrency:** `Server`, `Client`, `Session`, and `Stream`,
   including moved-from/broken states, shutdown wakeups, and no implicit
   reconnect or replay.
3. **Scheduling and limits:** `RouteOptions`, serialization domains and queue
   policies, backpressure, memory budgets, and statistics definitions.
4. **Wire and errors:** protocol v2 framing, request IDs, status semantics,
   stream framing, `ErrorCode`, and `system_code()` preservation.
5. **Distribution:** Linux/Windows support statements, CMake version and
   package metadata, static/shared consumers, and README/README.ko parity.
6. **Evidence:** deterministic unit/integration tests, sanitizer/TSan/fuzz
   smoke, bounded stress/soak, native/ARM64 measurements, and hosted Windows
   validation.

Historical 0.6 experiments, RC records, and migration notes remain useful
evidence and should be classified rather than deleted. Any cleanup of those
records belongs to a dedicated 1.0 documentation pass.

## Known limitations and deferred work

The following are intentional scope boundaries, not 0.9 bugs:

- Windows resource/HANDLE passing and Linux `SCM_RIGHTS` portability;
- Windows SID/token identity and a cross-platform credential model;
- generic `NativeHandle` abstractions;
- IOCP-specific optimization beyond the concrete 0.8 readiness backend;
- typed/declarative RPC, protocol v3, automatic retry/replay, and C/Python
  bindings.

The hosted Windows runner remains the authoritative Windows runtime/compiler
environment because the repository development image has no MSVC. Repeat the
Windows Core/Session/Simple and package matrix for every release candidate.

## 1.1 idea backlog

Potential ideas such as typed bindings, richer identity/resource capabilities,
and a completion-based Windows backend are intentionally backlog items. They
must not be implemented on the stabilization line without a new design and a
fresh compatibility/performance review.

## Release policy

Do not create a 0.9 tag or GitHub Release from this roadmap. First finish the
stabilization checks, record any remaining limitations, and obtain explicit
release approval. A future release note should link this document together
with the final Actions run and the relevant performance/soak evidence.
