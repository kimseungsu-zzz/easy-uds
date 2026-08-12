# 0.6 performance measurements

These are development measurements, not portable guarantees. Each run uses
the existing session benchmark on WSL2 with g++ `-O3`, one persistent session,
and the configured build-time spin window.

| Spin window | Requests | p50 | p95 | p99 | Throughput |
|---:|---:|---:|---:|---:|---:|
| 0 µs | 30,000 | 68.1 µs | 183.5 µs | 256.4 µs | 11.6k req/s |
| 10 µs | 12,000 | 70.6 µs | 149.7 µs | 235.1 µs | 11.9k req/s |
| 25 µs | 12,000 | 88.6 µs | 178.1 µs | 254.6 µs | 9.9k req/s |
| 50 µs | 12,000 | 41.6 µs | 112.9 µs | 187.7 µs | 18.4k req/s |
| 100 µs | 30,000 | 47.0 µs | 124.4 µs | 257.5 µs | 16.2k req/s |

The samples are noisy on a shared WSL2 host, but zero spin consistently loses the
low-latency path. The default remains 100 µs because it is a conservative choice
under tail-latency variation; 50 µs is a candidate for a future platform-specific
profile, not a public API change.

The benchmark also reports `getrusage()` user/system CPU time and voluntary /
involuntary context switches. `perf stat` and `strace -c` should be used on a
host where those tools are available for syscall, cache-miss, and branch-miss
counts. They are not available in the current WSL image.

The warmed-up allocation benchmark reports `0` ordinary heap allocations per
request in the current session fast path (5,000 requests). The standalone
false-sharing probe measured `5.31x` speedup from 64-byte padding on this WSL2
host (20 million relaxed increments per counter). That result is diagnostic only;
production state will be padded only after a workload-specific measurement.
