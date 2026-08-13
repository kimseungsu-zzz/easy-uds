# 0.6 performance measurements

These are development measurements, not portable guarantees. The spin sweep
below uses the session benchmark on WSL2 with g++ `-O3`, one persistent session,
50,000 requests, and each build-time spin window rebuilt separately
(2026-08-12). `EASY_UDS_TRACE_SPIN_MISS=ON` (a build-time diagnostic, default
OFF) reports how many requests fell through the spin window to the condvar wait.

| Spin window | p50 | p95 | p99 | Throughput | condvar fallback |
|---:|---:|---:|---:|---:|---:|
| 0 µs | 49.3 µs | 76.8 µs | 127.6 µs | 18.4k req/s | 99.99 % |
| 25 µs | 34.0 µs | 71.9 µs | 132.4 µs | 24.4k req/s | 16.42 % |
| 50 µs | 34.9 µs | 90.2 µs | 174.3 µs | 23.0k req/s | 5.24 % |
| 100 µs | 34.5 µs | 55.8 µs | 121.8 µs | 25.9k req/s | 0.58 % |
| 200 µs | 34.5 µs | 62.0 µs | 127.1 µs | 25.1k req/s | 0.074 % |
| 500 µs | 34.2 µs | 58.8 µs | 127.0 µs | 25.6k req/s | 0.026 % |

On this development host 100 µs is the sweet spot: p50 reaches the raw WSL
round-trip floor (~34 µs), p95/p99/throughput are the best of the sweep, and
the spin covers 99.42 % of responses (only 0.58 % fall through to the condvar).
Dropping to 50 µs raises fallback to 5.24 % and worsens the p95/p99 tail even
though p50 stays at the floor; raising to 200/500 µs buys little (0.07 % → 0.03 %)
at the cost of a worse p99. Zero spin falls through on essentially every request.
The residual ~2 futex calls and ~2 voluntary context switches per request at
100 µs are server-side (worker-pool queueing, reactor dispatch, reader-thread
scheduling), not the client response wait — the client spin is already near
optimal, so the next latency lever is server-side. The ARM64 final gate compared
the same c1 workload at 50 µs and 100 µs. The 50 µs candidate changed p50 by
less than 1% and left p99 effectively tied, but worsened p99.9 by 11% (39.4 µs
versus 35.5 µs). Together with the WSL fallback and tail result, this finalizes
100 µs as the cross-platform default. A runtime or architecture-specific public
knob is not justified.

The benchmark also reports `getrusage()` user/system CPU time and voluntary /
involuntary context switches. `strace -c` is used for syscall counts on the
development WSL2 image (the roadmap Phase 0 strace baseline below). `perf stat`
still needs a host where perf is available for cache-miss and branch-miss counts.

The warmed-up allocation benchmark reports `0` ordinary heap allocations per
request in the current session fast path (5,000 requests). The standalone
false-sharing probe measured `5.31x` speedup from 64-byte padding on this WSL2
host (20 million relaxed increments per counter). That result is diagnostic only;
production state will be padded only after a workload-specific measurement.

The file-backed zero-copy probe compares `sendfile()` and `splice()` with a
read/write copy for an 8 MiB payload over a Unix socketpair. Repeated WSL2
runs observed roughly `1.26x–2.59x` for `sendfile()` and `1.58x–2.88x` for
`splice()`; the result is host-sensitive. This is enough to keep the probe,
but not enough to add a public file-source API without an end-to-end framed
stream measurement.

## Native Linux final gate (2026-08-13)

GitHub Actions runs `31672749178` and `31673244625` executed the same release
binaries on hosted Ubuntu 24.04 x86_64 (4-vCPU AMD EPYC 7763) and ARM64
(4-vCPU Neoverse-N2). These are hosted-runner release gates, not an
architecture-only CPU comparison or portable guarantee. Their value is
identical commands, real Linux kernels, complete resource reporting, a repeat
after protocol hardening, and reproducible artifacts.

### End-to-end library paths

| Host / workload | Throughput | p50 | p99 | p99.9 | CPU-s / 1M |
|---|---:|---:|---:|---:|---:|
| x86_64 one-shot c1, empty | 15.1k req/s | 63.7 us | 110 us | 166 us | 79.5* |
| ARM64 one-shot c1, empty | 25.3k req/s | 37.7 us | 61.1 us | 118 us | 39.5* |
| x86_64 one-shot c1, 1 MiB | 2.10k req/s | 473 us | 528 us | 529 us | 750* |
| ARM64 one-shot c1, 1 MiB | 2.32k req/s | 431 us | 469 us | 476 us | 700* |
| x86_64 shared session c1 | 26.1k req/s | 38.4 us | 48.8 us | 59.8 us | 70.0 |
| ARM64 shared session c1 | 47.4k req/s | 21.0 us | 23.7 us | 34.9 us | 33.8 |
| x86_64 shared session c8 | 32.3k req/s | 232 us | 821 us | 3.01 ms | 107.5 |
| ARM64 shared session c8 | 42.2k req/s | 164 us | 784 us | 2.57 ms | 78.6 |
| x86_64 shared session c32 | 30.8k req/s | 1.00 ms | 2.48 ms | 3.97 ms | 123.2 |
| ARM64 shared session c32 | 36.0k req/s | 851 us | 2.56 ms | 4.68 ms | 106.0 |

`*` One-shot CPU is total `/usr/bin/time` user+system time divided by requests,
so it includes short process/setup overhead. Session CPU is the benchmark's
in-window `getrusage()` value. The x86_64 stream measured 6.19/5.65 GiB/s
upload/download; ARM64 measured 8.79/8.36 GiB/s. The ARM64 c64 shared-session
point remained stable at 34.6k req/s, 1.82 ms p50, 4.00 ms p99, and 111.7
CPU-s/1M.

### Allocation closure

The second native run measured ordinary heap operations on the warmed paths:

| Host | Path | Work | Allocations |
|---|---|---:|---:|
| x86_64 | warm Session | 20,000 requests | 0/request |
| ARM64 | warm Session | 20,000 requests | 0/request |
| x86_64 | serialized executor | 5,000 requests | 2.3332/request |
| ARM64 | serialized executor | 5,000 requests | 2.3334/request |
| x86_64 | 1 MiB stream | 50 exchanges | 20.5/MiB |
| ARM64 | 1 MiB stream | 50 exchanges | 20.5/MiB |

The latency-critical warm Session already performs no heap allocation per
request. Pooling the remaining serialized/stream allocations is rejected for
0.6: earlier allocator A/B showed no workload-level win, while ownership and
reclamation complexity would increase substantially.

### Session spin default

| ARM64 c1 | Throughput | p50 | p99 | p99.9 | CPU-s / 1M |
|---|---:|---:|---:|---:|---:|
| 100 µs (default) | 46.7k req/s | 21.217 µs | 24.673 µs | 35.465 µs | 33.99 |
| 50 µs candidate | 46.9k req/s | 21.097 µs | 24.616 µs | 39.401 µs | 33.67 |

The throughput, p50, p99, and CPU differences are below 1%; the 50 µs
candidate instead regresses p99.9 by about 11%. The default remains 100 µs,
with no public runtime option added.

### Process SHM is architecture/workload specific

| Host / payload | Path | Throughput | p50 | p99 | CPU-s / 1M | eventfd / exchange |
|---|---|---:|---:|---:|---:|---:|
| x86_64 / 4 KiB | direct + conditional | 2.74 GiB/s | 2.35 us | 3.52 us | 5.51 | 0.0095 |
| x86_64 / 4 KiB | socketpair | 0.354 GiB/s | 21.3 us | 38.4 us | 24.4 | n/a |
| ARM64 / 4 KiB | direct + conditional | 0.436 GiB/s | 17.1 us | 20.7 us | 12.4 | 2.00 |
| ARM64 / 4 KiB | socketpair | 0.501 GiB/s | 14.1 us | 21.1 us | 16.6 | n/a |
| x86_64 / 1 MiB | direct + always | 2.75 GiB/s | 475 us | 1.23 ms | 707 | 2.00 |
| x86_64 / 1 MiB | socketpair | 2.80 GiB/s | 676 us | 716 us | 901 | n/a |
| ARM64 / 1 MiB | direct + always | 3.36 GiB/s | 451 us | 885 us | 576 | 2.00 |
| ARM64 / 1 MiB | socketpair | 2.73 GiB/s | 694 us | 732 us | 917 | n/a |

The 256-iteration conditional window covers the hot 4 KiB exchange on x86_64
but almost never covers it on this ARM64 runner. At 1 MiB direct slots reduce
copy CPU and p50, while p99/throughput vary by host. This confirms the decision
to retain the result as an experiment rather than expose a 0.6 public transport.

### Native io_uring decision

| Host / load | Backend | Throughput | p50 | p99 | p99.9 | CPU-s / 1M |
|---|---|---:|---:|---:|---:|---:|
| x86_64 c2 | epoll | 64.1k req/s | 34.0 us | 41.7 us | 52.4 us | 22.7 |
| x86_64 c2 | io_uring | 66.8k req/s | 31.3 us | 44.9 us | 52.1 us | 22.8 |
| x86_64 c8 | epoll | 152.5k req/s | 37.6 us | 93.0 us | 2.94 ms | 16.2 |
| x86_64 c8 | io_uring | 110.2k req/s | 51.3 us | 145 us | 3.00 ms | 22.2 |
| x86_64 c32 | epoll | 157.1k req/s | 168 us | 428 us | 2.77 ms | 16.2 |
| x86_64 c32 | io_uring | 125.4k req/s | 254 us | 300 us | 335 us | 21.4 |
| ARM64 c2 | epoll | 138.8k req/s | 14.0 us | 18.8 us | 24.4 us | 10.2 |
| ARM64 c2 | io_uring | 120.5k req/s | 17.0 us | 23.8 us | 29.3 us | 11.2 |
| ARM64 c8 | epoll | 241.4k req/s | 23.8 us | 60.7 us | 2.42 ms | 8.35 |
| ARM64 c8 | io_uring | 183.5k req/s | 32.6 us | 52.9 us | 2.93 ms | 11.4 |
| ARM64 c32 | epoll | 256.4k req/s | 115 us | 236 us | 2.50 ms | 7.96 |
| ARM64 c32 | io_uring | 209.0k req/s | 152 us | 172 us | 205 us | 11.0 |

io_uring improves high-concurrency extreme tail on the c32 hosted runs, but
costs 18-28% throughput, 32-51% p50, and 32-38% CPU. The isolated c2 x86_64
point is approximately tied. Under the 0.6 gate (clear latency/CPU win without
a c1 regression), that trade is insufficient: production epoll remains the
lower-complexity and more CPU-efficient backend.

Both final dispatches passed. The repeated gate includes 20 complete x86_64
unit+stress repetitions, five on ARM64, ASan/UBSan, TSan, 200,000 protocol fuzz
executions, 20,000 stateful session fuzz executions, GCC/Clang static/shared
builds, and static/shared installed-package consumers.

## WSL reference baseline (2026-08-12)

Development station: i7-1260P, WSL2 (kernel 6.18.33-microsoft-standard), g++ 15.2,
CMake 4.2.3, `Release`, ninja. These are a shared-host validation and A/B
reference, not the canonical baseline; `ROADMAP_0.6.md` Phase 0 calls for a
re-run on a native Linux host before absolute numbers are used.

| Benchmark | Run | Throughput | p50 | p95 | p99 |
|---|---:|---:|---:|---:|---:|
| session (independent, c8) | 200,000 | 47.7k req/s | 125.6 µs | 396.2 µs | 620.4 µs |
| session (shared, c8) | 200,000 | 25.1k req/s | 278.6 µs | 697.2 µs | 997.6 µs |
| one-shot RPC (c8) | 100,000 | 41.5k req/s | 179.2 µs | 301.4 µs | 428.1 µs |
| warm session allocation | 20,000 | — | 1 heap alloc total (5e-05/req) | | |
| stream (64 KiB chunks, 1 GiB) | 1024 MiB | upload 10.0 GiB/s, download 10.3 GiB/s | | | |

Session resources (independent c8): user 19.3 s, system 19.6 s, voluntary
context switches 567 k, involuntary 25 k. Shared c8: voluntary 641 k, involuntary 76.

### Shared-session contention sweep (2026-08-13)

The `scripts/session_contention_sweep.sh` helper runs one shared `Session`
through c1/c2/c4/c8/c16/c32. A short WSL2 smoke run (2,000 total requests per
point, `session_idle_grace=0`) reproduced the expected contention curve:

| Callers | Throughput | p50 | p99 |
|---:|---:|---:|---:|
| 1 | 14.4k req/s | 53.6 us | 229.9 us |
| 2 | 20.9k req/s | 73.4 us | 309.7 us |
| 4 | 17.8k req/s | 202.3 us | 573.5 us |
| 8 | 19.2k req/s | 375.6 us | 1.10 ms |
| 16 | 19.7k req/s | 669.1 us | 2.41 ms |
| 32 | 26.0k req/s | 1.09 ms | 2.99 ms |

These numbers are diagnostic rather than a release baseline, but they confirm
that a single shared session becomes the limiting topology under high caller
counts. They selected sharded in-flight lookup as the next isolated experiment;
the sender path remains a separate future A/B.

### Shared-session in-flight sharding A/B (2026-08-13)

`EASY_UDS_TRACE_SESSION_CONTENTION` first separated send, caller-table,
reader-table, and per-slot lock waits. At c8 the single caller-side table lock
waited about 48 us per acquisition versus about 24 us for the send lock; at
c32 it reached about 141 us. Slot-lock waits stayed below 1 us. This selected
the pending table—not waiter notification—as the first isolated experiment.

The table was then split by request id with an atomic id allocator and
cache-line-separated shards. The following figures are medians of three WSL2
release runs with tracing disabled, 50,000 requests per concurrent point and
30,000 requests at c1:

| Load | Shards | Throughput | p50 | p99 | CPU-s / 1M requests |
|---:|---:|---:|---:|---:|---:|
| c1 | 1 | 10.9k req/s | 66.7 us | 355.7 us | 136.9 |
| c1 | 16 | 11.2k req/s | 65.8 us | 354.2 us | 134.5 |
| c8 | 1 | 13.7k req/s | 458.8 us | 2658.9 us | 302.7 |
| c8 | 16 | 21.5k req/s | 338.8 us | 1038.7 us | 236.9 |
| c32 | 1 | 19.5k req/s | 1326.2 us | 5057.4 us | 279.4 |
| c32 | 16 | 34.6k req/s | 827.5 us | 2496.2 us | 204.7 |

Scorecard: at c8, 16 shards improve throughput 57%, p50 26%, p99 61%, and
CPU cost 22%; at c32 they improve throughput 78%, p50 38%, p99 51%, and CPU
cost 27%. The c1 medians are effectively unchanged. Eight shards retained
noticeably more contention at c32 (22.8k req/s median and 3.69 ms p99), so the
default is 16. Decision: **ACCEPT**. The build-time shard count remains tunable
for repeatable experiments; the public API and protocol are unchanged.

### Shared-session sender queue/batching A/B (2026-08-13)

After sharding removed the table bottleneck, the next isolated experiment
replaced the direct `send_mutex + sendmsg` path with an intrusive MPSC queue,
one sender thread, and gathered batches capped at 1, 4, or 8 request frames.
The implementation passed the normal unit/stress suite, but release sweeps at
c1/c2/c4/c8/c16/c32 showed no stable end-to-end win. Batch 8 was the best
queued variant; medians of three 30,000-request confirmation runs were:

| Load | Path | Throughput | p50 | p99 | CPU-s / 1M requests |
|---:|---|---:|---:|---:|---:|
| c1 | direct mutex | 9.15k req/s | 79.8 us | 309 us | 165.1 |
| c1 | sender, batch 8 | 7.06k req/s | 115.5 us | 384.6 us | 193.0 |
| c8 | direct mutex | 20.2k req/s | 371.6 us | 983.9 us | 249.0 |
| c8 | sender, batch 8 | 17.6k req/s | 433.6 us | 1009.9 us | 280.5 |
| c32 | direct mutex | 30.3k req/s | 990.2 us | 2307.6 us | 226.9 |
| c32 | sender, batch 8 | 26.9k req/s | 1149.7 us | 2400.7 us | 253.2 |

Diagnostic runs showed batch 8 only combined about 2.3 frames at c8 and 3.7
at c32. The extra sender handoff, condition-variable wake, and caller wait cost
more than the saved send serialization: throughput fell 11-23%, p50 regressed
16-45%, CPU cost rose 12-17%, and voluntary context switches increased. Batch
1 was worse and batch 4 did not change the conclusion. Decision: **REJECT**.
The queued sender implementation and knob were removed; this document retains
the result, and the direct mutex remains the production path.

### Shared-memory transport probe (2026-08-13)

The standalone `memfd + eventfd` SPSC probe measured, on the same WSL2 host,
about 5.0 GiB/s versus 2.1 GiB/s for a 4 KiB socketpair payload, and 9.6 GiB/s
versus 2.6 GiB/s for a 64 KiB payload. This is a raw fixed-payload transport
comparison with one producer and one consumer; it excludes crash recovery,
multi-producer ordering, leases, and application framing. It is therefore
evidence for an experimental data-plane direction, not a public API decision.

The follow-up `easy_uds_shm_framed_probe` adds the production v2 20-byte
big-endian header and validates an echo response for every request. It compares
a two-ring memfd/eventfd data plane (with SCM_RIGHTS setup) against the same
framed request/response over a `SOCK_STREAM` socketpair. Run it at 4 KiB,
64 KiB, and 1 MiB before considering a shared-memory transport API; it still
omits reactor scheduling, handler work, multiplexing, and failure recovery.

One WSL2 smoke sweep (2026-08-13, g++ `-O3`, 1000/100/10 rounds) produced:

| Payload | Shared p50 / p99 | Socketpair p50 / p99 | Shared throughput | Socketpair throughput |
|---:|---:|---:|---:|---:|
| 4 KiB | 91.7 / 208 us | 44.0 / 226 us | 0.079 GiB/s | 0.104 GiB/s |
| 64 KiB | 236 / 386 us | 359 / 562 us | 0.503 GiB/s | 0.337 GiB/s |
| 1 MiB | 2.19 / 2.51 ms | 1.34 / 2.11 ms | 0.862 GiB/s | 1.073 GiB/s |

The crossover is workload-sensitive: shared memory helps this 64 KiB point,
but its extra ring copies and eventfd wakeups lose at 4 KiB and 1 MiB. These
numbers are an experiment result, not a transport selection or a release
performance guarantee.

#### Process/direct-slot/conditional-wakeup follow-up (2026-08-13)

`easy_uds_shm_process_probe` repeats the framed exchange between separate
`fork()` processes. The parent creates two memfds and two eventfds after the
fork and transfers all four descriptors with `SCM_RIGHTS`, so the data-plane
setup is no longer an in-process approximation. It compares three ring paths:

1. private application buffers copied into mapped slots, with one eventfd write
   on every publish;
2. direct mapped-slot write/read, still with an eventfd write every publish;
3. direct slots with a sleeping-consumer handshake, signaling only when the
   consumer cannot observe work after its spin window.

One WSL2 `g++ -O3` sweep at 2,048 spin iterations with the corrected request
layout (20-byte header, 4-byte route, body) produced these representative
points:

| Payload | Path | p50 | p99 | p99.9 | Throughput | CPU-s / 1M | eventfd / exchange |
|---:|---|---:|---:|---:|---:|---:|---:|
| 64 B | copy + always | 32.8 us | 137.5 us | 245 us | 0.0038 GiB/s | 32.3 | 2.0 |
| 64 B | direct + conditional | 0.47 us | 0.54 us | 10.5 us | 0.275 GiB/s | 1.17 | 0.00005 |
| 4 KiB | copy + always | 42.2 us | 111 us | 229 us | 0.172 GiB/s | 34.6 | 2.0 |
| 4 KiB | direct + conditional | 2.49 us | 2.70 us | 13.1 us | 2.91 GiB/s | 5.28 | 0 |
| 64 KiB | copy + always | 73.2 us | 246 us | 320 us | 1.19 GiB/s | 88.9 | 2.0 |
| 64 KiB | direct + conditional | 26.1 us | 97.4 us | 184 us | 3.90 GiB/s | 62.4 | 0.01 |
| 1 MiB | direct + always | 501 us | 869 us | 869 us | 2.86 GiB/s | 654 | 2.0 |
| 1 MiB | direct + conditional | 567 us | 1097 us | 1097 us | 2.65 GiB/s | 881 | 2.0 |
| 1 MiB | socketpair | 690 us | 1377 us | 1377 us | 2.40 GiB/s | 960 | n/a |
| 4 MiB | direct + conditional | 3.76 ms | 4.26 ms | 4.26 ms | 1.62 GiB/s | 4898 | 2.0 |
| 4 MiB | socketpair | 4.47 ms | 5.70 ms | 5.70 ms | 1.60 GiB/s | 5849 | n/a |

Removing the intermediate payload copies is useful, but conditional wakeup is
the decisive hot-path change below 64 KiB. At 1 MiB and above, frame fill and
validation exceed the spin window and eventfd returns to two writes per
exchange; the advantage narrows and becomes run-sensitive.

A separate spin sweep compared 0/64/256/1024/2048 iterations. A 256-iteration
window retained the 64-byte hot p50/p99 result (0.44/0.50 us in that sweep)
while using about 69% less CPU than 2048 iterations when the responder delayed
every reply by 2 ms. The default probe value is therefore 256. The idle test
also shows why this is not a general transport decision: any spin window burns
CPU while waiting for slow application work.

The zero-spin stress initially exposed a real lost wakeup in the experimental
handshake. A consumer used a plain store to advertise sleep while the producer
used an exchange, permitting both sides to miss each other. Pairing producer
and consumer RMW exchanges fixed the ordering; zero-spin then completed
100,000 exchanges, and the ASan/UBSan boundary runs remained clean.

Decision: **KEEP AS AN EXPERIMENT, DO NOT ADD A PUBLIC SHM TRANSPORT IN 0.6.x**.
The fast result is compelling for a continuously hot, trusted SPSC data channel,
but ownership lifetime, peer crash recovery, ring leases, backpressure, and
multi-producer ordering would add substantially more complexity. The result is
enough to inform a future opt-in design without expanding the final 0.6 API.

### Basic io_uring versus epoll A/B (2026-08-13)

The original io_uring echo probe had no matched epoll baseline and assumed that
every send and receive completed all eight bytes. The final probe now uses one
correctness-checking exact-I/O client harness for both backends, handles partial
server I/O, keeps pending io_uring buffers alive through ring teardown, and
reports p99.9 plus process CPU and context switches. The server work is kept
deliberately equivalent: re-armed accept followed by receive and echo send. It
is a transport ceiling, not the full production parser/dispatcher.

Medians of three WSL2 `g++ -O3` runs were:

| Load | Backend | Throughput | p50 | p99 | p99.9 | CPU-s / 1M |
|---:|---|---:|---:|---:|---:|---:|
| c1 | epoll | 18.9k req/s | 38.7 us | 255 us | 502 us | 44.3 |
| c1 | io_uring | 20.1k req/s | 41.6 us | 234 us | 413 us | 42.3 |
| c2 | epoll | 49.8k req/s | 27.6 us | 182 us | 331 us | 26.4 |
| c2 | io_uring | 27.1k req/s | 54.3 us | 267 us | 507 us | 44.1 |
| c8 | epoll | 51.3k req/s | 126 us | 587 us | 1110 us | 32.0 |
| c8 | io_uring | 35.9k req/s | 211 us | 584 us | 1063 us | 39.0 |
| c32 | epoll | 63.5k req/s | 411 us | 1424 us | 2423 us | 25.2 |
| c32 | io_uring | 31.9k req/s | 877 us | 3190 us | 4465 us | 47.0 |

The c1 result is mixed and within this host's large run-to-run variance. From
c2 upward the basic io_uring state machine loses decisively: at c32 throughput
falls about 50%, p50 more than doubles, and CPU cost rises about 86%.

`strace -f -c` at c8/16,000 requests counted about 4.13 steady-state calls per
request for epoll (client and server send/receive plus epoll wait), versus 2.25
for io_uring (client send/receive plus `io_uring_enter`). Timing under strace is
not comparable because interception penalizes the syscall-heavier backend, but
the counts establish the useful negative result: fewer syscalls did not produce
lower untraced latency or CPU cost. The completion handoff remains the dominant
cost for this tiny synchronous exchange.

Decision: **REJECT A BASIC IO_URING BACKEND FOR 0.6.x; KEEP PRODUCTION EPOLL**.
Multishot accept cannot affect the steady-state request result, provided buffers
do not help fixed per-connection eight-byte storage, and zero-copy send is aimed
at large payloads rather than this RPC path. Those features may be isolated in
a future workload-specific experiment, but the result does not justify reactor
complexity in the closing 0.6 release.

### Read-ahead batch size sweep (2026-08-12)

`reactor_read_batch_size` (256 KiB default) caps how much the reactor parses
per connection per readiness event. The RPC benchmark gained an optional
payload-size argument (`iterations concurrency payload_bytes`, handler echoes
the payload) to exercise large frames. One-shot RPC, 300 requests:

| Batch | 1 MiB p50 | 1 MiB p99 | 1 MiB throughput | tiny c8 p50 | tiny c8 throughput |
|---:|---:|---:|---:|---:|---:|
| 64 KiB | 648.6 µs | 1714.9 µs | 1297 req/s | 195.8 µs | 37.2k req/s |
| 256 KiB (default) | 675.6 µs | 1608.7 µs | 1427 req/s | 179.3 µs | 40.3k req/s |
| 1 MiB | 1358.2 µs | 2455.8 µs | 706 req/s | 177.6 µs | 39.5k req/s |

The 1 MiB cap is a clear regression on large frames (~2x): a large batch
balloons the per-connection `pending` buffer and its copy/retention cost per
event. 64 KiB costs ~9 % of large-frame throughput. Tiny RPCs are unaffected.
Decision: keep the 256 KiB default; no change.

### False-sharing padding A/B (2026-08-12)

`Connection` carries adjacent atomics written by both the reactor and the
worker pool (`last_io_progress`, `last_output_progress`, `inflight_requests`,
`inflight_request_bytes`, `queued_output_bytes`). A variant with `alignas(64)`
on all five was compared with the packed layout on the shared-session benchmark,
200,000 requests, 16 concurrent callers on one session, three runs each:

| Layout | throughput (median) | p50 | p99 |
|---|---:|---:|---:|
| packed (current) | 26.6k req/s | 504.6 µs | 2021 µs |
| 5 × `alignas(64)` | 26.7k req/s | 505.3 µs | 2016 µs |

No measurable difference; run-to-run variance (~5 %) exceeds any padding
effect on this host. Decision: do not adopt padding — production state stays
packed until a workload-specific measurement shows the gap. This A/B is
WSL-only evidence; the standalone synthetic probe's 5.31x does not reproduce
under real multiplexed traffic on this host.

### Allocation by path (2026-08-12)

The allocation benchmark gained `stream` and `serialized` modes in addition to
the original warm session fast path (operator-new override counting both
in-process client and server). WSL2, `-O3`:

| Path | Workload | Allocations |
|---|---:|---:|
| session fixed RPC | 20,000 requests | 8 total (0.0004/req) |
| serialized route | 20,000 requests | 2.33/req |
| streamed 1 MiB exchange | 100 exchanges | 15.76/MiB (~1 per 64 KiB chunk) |

The fixed fast path is arbitrarily close to zero. Stream allocation scales with
transferred bytes (~1 heap alloc per 64 KiB chunk) and is not a bottleneck at
the measured 10 GiB/s. Serialized requests pay a small per-request constant
(the serialized-job enqueue copies request strings) on a low-frequency,
exclusive-executor path by design. Decision: no object pool or custom allocator
is warranted; nothing adopts.

### Continuation fast path: ON vs OFF (2026-08-12)

The session benchmark gained an optional `grace_ms` argument to disable the
worker-lease continuation (`session_idle_grace = 0`). `strace -f -c`, 3,000
sequential requests on one session:

| Metric | Continuation ON (1 ms) | Continuation OFF (0) |
|---|---:|---:|
| futex | 3.05 /req | **6.06 /req (2x)** |
| epoll_wait | ~0 /req | 1.00 /req |
| poll | 2.00 /req | 1.00 /req |
| epoll_ctl | 2.00 /req | 2.00 /req |
| recvfrom | 3.00 /req | 3.00 /req (3000 EAGAIN) |
| sendmsg | 2.00 /req | 2.00 /req |
| p50 / p99 (30k) | 37.7 / 158.6 µs | 75.6 / 525.7 µs |

The continuation fast path is already sustained multi-followup (the worker
re-acquires the lease and loops across consecutive requests) and is worth a
2x latency / 2x futex saving plus a near-zero `epoll_wait`. The residual
per-request costs (2 `epoll_ctl`, the `connections_mutex` in lease+rearm, 1
`poll` grace wait) sum to a small share of syscall time; skipping the rearm
for the strictly-sequential case would save ~2 % of syscall time at the risk
of serializing pipelined bursts. Decision: no continuation redesign — the
sequential path is already at the raw round-trip floor. The remaining latency
headroom is the multiplexed/shared-session path (measured p50 ~500 µs vs
~35 µs sequential), not sequential RPC.

### Framed zero-copy gate (2026-08-12)

`easy_uds_zero_copy_probe` gained a `framed` pass that mirrors the real wire
protocol (20-byte header + 64 KiB payload per frame) on top of the three raw
transports. This asks whether the socketpair-only speedups carry through a
framed end-to-end stream.

| Payload | Pass | sendfile vs read/write | splice vs read/write |
|---|---:|---:|---:|
| 8 MiB | raw | 1.80x | 1.92x |
| 8 MiB | framed | 1.10x | 1.48x |
| 64 MiB | raw | 1.68x | 2.21x |
| 64 MiB | framed | 1.72x | 1.82x |

The 20-byte header adds ~0.03 % of wire bytes, so the loss is not framing
overhead: capping each payload chunk at 64 KiB forces many small `sendfile` /
`splice` calls and shrinks the raw-mode win. Crucially, this probe compares
`sendfile`/`splice` only against a plain `read`+`write()` copy baseline
(~2.6–3 GiB/s on this host) — not against the library's gathered `sendmsg()`
path. That baseline is what makes the probe show a "win".

An in-library `sendfile`-backed file-stream (the 0.6.4 candidate) was then
measured through the real non-blocking stream path and **rejected**:

| Path (512 MiB stream) | Download |
|---|---:|
| callback `StreamReader` + gathered `sendmsg` | ~9.8 GiB/s |
| `sendfile`, non-blocking socket (1 MiB frames) | ~1.3–1.6 GiB/s |
| `sendfile`, temporarily blocking socket | ~2.3 GiB/s |
| forced read/loop fallback | ~1.5–2.2 GiB/s |

Standalone on this host a memfd read is ~5.5 GiB/s and blocking `sendfile`
~4.2 GiB/s, so the file→socket path is slower than the user-space gather path
before any non-blocking penalty is added. A file-source API is a 4–7x
regression here. Decision: the 0.6.4 file-stream API is **rejected**; the
zero-copy goal moves beyond 0.6 to a workload-specific io_uring experiment,
where `IORING_OP_SENDFILE` can be measured without the temporary blocking-mode
workaround. The framed and
raw probe passes remain as documentation of the naive transport result.

### recvmsg-with-control read path (2026-08-12)

The reactor read-ahead switched from `recv` to `recvmsg` with a one-descriptor
control buffer so SCM_RIGHTS descriptors survive read-ahead batches. Session
A/B on this host (200k, c8, 3 runs each): `recvmsg` 44.7k / 42.9k / 43.7k vs
plain `recv` 41.6k / 42.8k / 42.3k req/s — no measurable regression; the earlier
single-run 14% gap was shared-host noise.

### Syscalls per session request

`strace -f -c -S calls` on the session benchmark, 3,000 requests, concurrency 1.
Startup/setup syscalls excluded; per-request steady-state counts:

| syscall | calls / 3k req | /req | share of syscall time |
|---|---:|---:|---:|
| futex | 9,107 | ~3.0 | 50.7 % (biggest cost) |
| recvfrom | 9,002 | ~3.0 | 6.6 % |
| poll | 6,001 | ~2.0 | 18.8 % |
| epoll_ctl | 6,003 | ~2.0 | 1.8 % |
| sendmsg | 6,000 | ~2.0 | 2.5 % |
| epoll_wait | 3 | ~0.001 | 19.5 % |

~12 syscalls/request. `futex` dominates syscall time (the client spin window is
not eliminating the session lock/condvar waits; the server worker-pool and
per-connection queues also touch futex). `poll` at 2/request and `epoll_ctl` at
2/request are reduction candidates on the reader wait and connection rearm
paths. `epoll_wait` at ~0/request confirms the continuation fast path keeps the
reactor mostly dormant for a single sequential session.
