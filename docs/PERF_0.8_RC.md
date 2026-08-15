# easy-uds 0.8 RC performance record

This is a repeatable Linux reference run for the 0.8 candidate. It is not a
cross-platform parity claim: Windows and ARM64 numbers must come from their
respective hosted jobs.

## Environment

- WSL2 x86_64, Linux 6.18.33.2-microsoft-standard-WSL2
- Intel Core i7-1260P, 16 logical CPUs
- GCC 15.2.0, CMake 4.2.3, Release build
- `scripts/final_linux_benchmarks.sh /tmp/easyuds-rc-gate-off`
- 2026-08-15 UTC

## Observed reference values

| Workload | Throughput | p50 | p99 | CPU-s / 1M |
|---|---:|---:|---:|---:|
| One-shot c1, empty | 15.1k req/s | 56.4 µs | 202.0 µs | — |
| One-shot c8, empty | 46.5k req/s | 161.3 µs | 345.7 µs | — |
| Shared Session c1 | 24.3k req/s | 35.1 µs | 153.4 µs | 61.6 |
| Shared Session c8 | 34.0k req/s | 219.2 µs | 625.3 µs | 170.1 |
| Shared Session c32 | 62.4k req/s | 461.5 µs | 1,528.6 µs | 154.4 |
| Shared Session c64 | 75.1k req/s | 758.5 µs | 2,455.3 µs | 144.4 |

Streaming reached 9.7 GiB/s upload and 10.8 GiB/s download for the 256 MiB
workload. The warmed Session allocation probe reported 3 allocations over
20,000 requests (0.00015/request); this is the setup-inclusive probe and does
not change the dedicated steady-state no-allocation contract.

The optional io_uring A/B was unavailable in this WSL environment. No
optimization decision is based on that skipped probe. ARM64 and native Linux
reruns remain workflow-controlled measurements, and Windows has no local
compiler in this development environment.
