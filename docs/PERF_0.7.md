# 0.7 performance regression log

0.7 is a usability/API line, not a new performance-experiment line. Every
entry compares against tag `v0.6.4` with the same compiler, host, build flags,
workload, and repeated runs. Absolute values are development measurements, not
portable guarantees.

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
