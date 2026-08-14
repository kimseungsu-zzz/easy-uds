# 0.7 performance regression log

0.7 is a usability/API line, not a new performance-experiment line. Every
entry compares against tag `v0.6.4` with the same compiler, host, build flags,
workload, and repeated runs. Absolute values are development measurements, not
portable guarantees.

## Functional public/internal layout (2026-08-14)

Change: split the public umbrella into self-contained feature headers and move
implementation files under client, server, protocol, reactor, and detail
responsibility directories. Client, Session, and shared stream transport are
separate translation units. The small fixed-request validation and frame-write
helpers remain inline in an internal header so the compiler retains the same
hot-path optimization opportunity.

### Default hot-path A/B

WSL2, g++ 15.2, CMake Release, empty payload, one shared Session. Runs alternate
old/new order to reduce host drift. c1 is the median of five 100,000-request
runs per revision; c8 is the median of four 100,000-request runs per revision.
The baseline is pre-layout commit `67ce0ee`.

| Workload / revision | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| Session c1 / `67ce0ee` | 19.29k req/s | 38.969 us | 198.374 us | 77.26 |
| Session c1 / functional layout | 19.30k req/s | 38.988 us | 197.881 us | 77.18 |
| c1 delta | +0.0% | +0.0% | -0.2% | -0.1% |
| Session c8 / `67ce0ee` | 26.12k req/s | 287.625 us | 819.739 us | 204.43 |
| Session c8 / functional layout | 26.05k req/s | 287.053 us | 829.511 us | 204.44 |
| c8 delta | -0.3% | -0.2% | +1.2% | +0.0% |

Every median is effectively tied and remains well inside the 0.7 gate. The
organization-only change is retained.

### Compatibility and correctness gates

- All 195 previously exported dynamic symbols are present with exactly the
  same mangled names; the layout introduced no additional exported symbol.
- Size and alignment are identical for 11 public types: peer credentials,
  Request, Response, StreamResponse, both option structs, Client, Session,
  Server, BorrowedFd, and OwnedFd. Protocol v2 is unchanged.
- Every installed public header compiles independently under C++17. Static and
  shared package consumers use the feature headers without the umbrella.
- Release/Werror unit and stress, ASan/UBSan, TSan, and every portable
  experimental probe pass after the move.

## Explicit Session state (2026-08-14)

Change: add `Session::status()` and `valid()` as lock-free snapshots of the
existing atomic broken flag, with an explicit moved-from state. The methods
are observer-only: `request()` gained no new branch, lock, allocation, syscall,
or stored field.

### Default hot-path A/B

WSL2, g++ 15.2, CMake Release, empty payload. Each row is the median of three
alternating runs from separately built pre-state-API `1e06934` and current
trees. Each run uses 30,000 requests and one shared Session.

| Workload / revision | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| Session c1 / `1e06934` | 23.82k req/s | 34.964 us | 160.601 us | 63.27 |
| Session c1 / state API | 23.57k req/s | 35.107 us | 151.746 us | 63.29 |
| c1 delta | -1.0% | +0.4% | -5.5% | +0.0% |
| Session c8 / `1e06934` | 33.63k req/s | 221.205 us | 638.844 us | 171.15 |
| Session c8 / state API | 33.38k req/s | 222.230 us | 656.057 us | 172.22 |
| c8 delta | -0.7% | +0.5% | +2.7% | +0.6% |

Every median remains inside the 0.7 gate. Because the benchmark does not call
the new observers and the request path is byte-for-byte source-equivalent,
the variation is scheduling noise; the explicit state API is retained.

### Correctness gates

- Active, timeout-broken, peer-close-broken, moved-from, move-construction,
  and move-assignment states have dedicated assertions.
- An observer repeatedly calls both state methods while eight threads share
  the Session; the Release, ASan/UBSan, and TSan suites pass.
- Static and shared Werror builds, stress tests, and installed-package
  consumers pass. The consumer checks the public return types and `noexcept`
  contract.

## Semantic error model (2026-08-13)

Change: operational exceptions now use one `Error` type with an easy-uds
category and an independently preserved OS `system_code()`. Protocol/limit
failures receive stable meanings, while local contract exceptions remain
standard C++ types. Category lookup and message construction occur only on
throw paths; successful I/O retains the same syscalls, locks, allocations, and
branches.

### Default hot-path A/B

WSL2, g++ 15.2, CMake Release, empty payload. Each row is the median of three
alternating runs from separately built pre-error-model `1cbf856` and current
trees. Session runs use 30,000 requests and one shared Session; one-shot runs
use 10,000 requests.

| Workload / revision | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| Session c1 / `1cbf856` | 24.58k req/s | 35.025 us | 143.193 us | 61.55 |
| Session c1 / semantic errors | 24.01k req/s | 34.988 us | 142.979 us | 62.51 |
| c1 delta | -2.3% | -0.1% | -0.1% | +1.6% |
| Session c8 / `1cbf856` | 32.70k req/s | 226.757 us | 683.244 us | 172.99 |
| Session c8 / semantic errors | 32.75k req/s | 226.778 us | 682.860 us | 173.27 |
| c8 delta | +0.2% | +0.0% | -0.1% | +0.2% |
| One-shot c1 / `1cbf856` | 14.95k req/s | 57.314 us | 181.125 us | n/a |
| One-shot c1 / semantic errors | 14.95k req/s | 57.235 us | 177.032 us | n/a |
| one-shot delta | +0.0% | -0.1% | -2.3% | n/a |

Every median remains inside the 0.7 gate. The c1 throughput/CPU variation is
not accompanied by a latency regression and is within the noise seen across
the alternating WSL runs; the exception-only abstraction is retained.

### Correctness gates

- Release/Werror static and shared unit/stress suites passed.
- ASan/UBSan unit and stress binaries passed with leak detection.
- Static and shared installed-package consumers caught a library-thrown
  `Error`, compared its DSO-owned category, and inspected its original errno.
- Timeout, closed Session, protocol, too-large, busy, invalid-FD, unavailable,
  human-readable context, and `std::system_error` catch compatibility have
  dedicated regressions.

## Typed FD ownership (2026-08-13)

Change: replace raw server-side `Request::fd` ownership with a one-`int`,
move-only `OwnedFd`; make the client input an explicit `BorrowedFd`. Request
queue moves now transfer ownership automatically, and normal, rejected,
expired, disconnected, and exception paths share the same destructor cleanup.
The wrapper adds no allocation, lock, or syscall unless a handler explicitly
calls `duplicate()`.

### Default hot-path A/B

WSL2, g++ 15.2, CMake Release, empty payload. Each row is the median of three
alternating runs from separately built `v0.6.4` and current trees. Session runs
use 30,000 requests and one shared Session; one-shot runs use 10,000 requests.

| Workload / revision | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| Session c1 / v0.6.4 | 25.50k req/s | 34.929 us | 132.185 us | 59.45 |
| Session c1 / typed FD | 25.52k req/s | 34.507 us | 125.268 us | 58.91 |
| c1 delta | +0.1% | -1.2% | -5.2% | -0.9% |
| Session c8 / v0.6.4 | 33.81k req/s | 218.234 us | 646.283 us | 168.26 |
| Session c8 / typed FD | 33.93k req/s | 217.256 us | 664.320 us | 168.68 |
| c8 delta | +0.3% | -0.4% | +2.8% | +0.3% |
| One-shot c1 / v0.6.4 | 14.96k req/s | 57.075 us | 181.735 us | n/a |
| One-shot c1 / typed FD | 15.38k req/s | 58.311 us | 185.155 us | n/a |
| one-shot delta | +2.8% | +2.2% | +1.9% | n/a |

All medians remain inside the 0.7 gate. The extra invalid-descriptor branch in
the request destructor is below measurement noise; the ownership abstraction
is retained.

### Correctness gates

- Release/Werror unit and stress suites passed.
- ASan/UBSan unit and stress binaries passed with leak detection (the direct
  unit run was used because the instrumented CTest run hit its 20-second
  harness limit without a sanitizer report).
- Static and shared install-package consumers built and ran with the installed
  `fd.hpp`.
- Ownership transfer, invalid duplication, caller retention, close-on-exec,
  explicit retained lifetime, rejected frame, and repeated leak paths have
  dedicated regressions.

## Strict partial-request accounting (2026-08-13)

Change: when `ServerOptions::max_total_inflight_bytes` is nonzero, a validated
fixed request or stream route reserves its declared route+body bytes before
parser buffers are allocated. The reservation transfers to queued/executing
work and is released by completion, enqueue failure, timeout, protocol error,
or partial connection close. A peer that cannot reserve pauses `EPOLLIN`; a
global low-water release evaluates all paused peers.

The default remains `0`. In that mode the 0.6.4 worker continuation and parser
read-ahead fast paths remain enabled. The strict mode deliberately routes
Session continuation reads through reactor admission so a worker cannot
bypass the aggregate cap.

### Default hot-path A/B

WSL2, g++ 15.2, `-O3 -DNDEBUG`, one shared Session, c1, 30,000 requests per
run. Values are medians of three back-to-back runs from separately built
`v0.6.4` and current trees.

| Revision / load | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| v0.6.4 / c1 | 22.66k req/s | 35.029 us | 170.186 us | 66.51 |
| 0.7 strict-budget change, option disabled / c1 | 23.00k req/s | 34.973 us | 168.363 us | 65.64 |
| c1 delta | +1.5% | -0.2% | -1.1% | -1.3% |
| v0.6.4 / c8 | 29.64k req/s | 250.558 us | 747.290 us | 186.57 |
| 0.7 strict-budget change, option disabled / c8 | 31.43k req/s | 235.082 us | 703.137 us | 179.10 |
| c8 delta | +6.0% | -6.2% | -5.9% | -4.0% |

The differences are measurement noise in the favorable direction. Decision:
the first 0.7 correctness abstraction passes the 0.6.4 regression gate.

### Correctness gates

- Release unit suite: 38 tests passed, including a header-only partial request
  that reserves the full cap, backpressures another connection, and resumes it
  after close.
- Concurrent stress: passed.
- ASan/UBSan with leak detection: passed.
- TSan: passed.

Native x86_64 and ARM64 numbers remain required before a 0.7 release; this
development A/B protects the default hot path during Phase 1.

### Native x86_64 / ARM64 gate

Workflow dispatch `31679006335` ran the 0.7 tree with the same scripts and
hosted runner classes used by the 0.6.4 closing gate `31672749178`. Separate
hosted runs are a release regression signal rather than a precise CPU A/B.

| Host / load | Revision | Throughput | p50 | p99 | CPU-s / 1M |
|---|---|---:|---:|---:|---:|
| x86_64 c1 | v0.6.4 | 26.06k req/s | 38.372 us | 48.751 us | 70.01 |
| x86_64 c1 | 0.7 Phase 1 | 25.77k req/s | 38.152 us | 50.625 us | 70.21 |
| x86_64 c8 | v0.6.4 | 32.30k req/s | 232.255 us | 821.487 us | 107.46 |
| x86_64 c8 | 0.7 Phase 1 | 32.59k req/s | 222.595 us | 867.075 us | 104.69 |
| ARM64 c1 | v0.6.4 | 47.41k req/s | 21.033 us | 23.745 us | 33.78 |
| ARM64 c1 | 0.7 Phase 1 | 48.31k req/s | 20.623 us | 23.111 us | 32.91 |
| ARM64 c8 | v0.6.4 | 42.16k req/s | 164.182 us | 783.521 us | 78.57 |
| ARM64 c8 | 0.7 Phase 1 | 42.34k req/s | 162.806 us | 804.146 us | 77.87 |

Every c1/c8 metric remains inside the roadmap gate. The c32/c64 points also
remain within about 2% for throughput, p50, p99, and CPU cost. The dispatch
additionally passed 20 x86_64 and five ARM64 unit/stress repetitions, the
longer protocol/session fuzz budgets, ASan/UBSan, TSan, experiments, and
static/shared package consumers.

## RequestContext foundation (2026-08-14)

Change: add opt-in contextual fixed handlers through `RouteOptions`, carrying
first-byte arrival, absolute deadline, and live cooperative-stop observations.
The existing `Handler(const Request&)` path does not construct a context or
allocate. Its immutable route entry keeps the original size and reuses the
existing serialized flag byte for a contextual bit, so the basic invocation
adds one predictable branch without reading the context callback storage.

### Basic-handler hot-path A/B

WSL2, g++ 15.2, CMake Release, empty payload, one shared Session. Runs alternate
old/new order. c1 is the median of five 100,000-request runs per revision; c8
is the median of four 100,000-request runs per revision. The baseline is the
pre-context layout commit `7d30528`.

The host showed visible run-to-run load drift, including one slow run on each
side of different pairs. No sample was discarded; the repeated median is the
gate value.

| Workload / revision | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| Session c1 / `7d30528` | 17.48k req/s | 44.877 us | 200.480 us | 81.85 |
| Session c1 / RequestContext tree, basic handler | 17.69k req/s | 45.043 us | 199.220 us | 81.30 |
| c1 delta | +1.2% | +0.4% | -0.6% | -0.7% |
| Session c8 / `7d30528` | 24.84k req/s | 295.827 us | 917.577 us | 203.70 |
| Session c8 / RequestContext tree, basic handler | 24.51k req/s | 301.242 us | 912.943 us | 205.95 |
| c8 delta | -1.3% | +1.8% | -0.5% | +1.1% |

Every basic-handler median remains well inside the 0.7 gate. Decision: keep
the explicit context path and its single tagged-entry branch. The basic
`Request` layout, protocol v2, socket framing, and Session path are unchanged.

### Correctness gates

- Exact, longest-prefix, and serialized contextual handlers have dedicated
  registration and invocation regressions.
- Context request id and peer match `Request`; arrival and deadline use the
  server's steady clock and request-timeout boundary.
- Disabled deadlines return an empty optional. Elapsed deadlines, peer
  disconnect, and server shutdown independently make the cooperative stop
  observation true.
- `RequestContext` is non-copyable/non-movable and its documented lifetime is
  exactly one callback. Installed headers compile it independently.
- Release/Werror static and shared unit/stress suites passed. Both installed
  package variants linked all three contextual registration overloads.
- ASan/UBSan with leak detection and TSan passed the complete unit and stress
  binaries. Existing public object layouts and protocol v2 are unchanged.
