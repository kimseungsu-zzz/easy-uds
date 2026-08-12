# 0.6 experimental probes

The standalone probe targets do not change the default epoll backend. They
answer whether Linux/UDS capabilities are available before we consider a
production design. The separately documented `request_fd()` API is an
experimental protocol-v2 feature; the probes themselves are not required by
the library's normal build.

Build them with:

```sh
cmake -S . -B build-experiments \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_BUILD_EXPERIMENTS=ON
cmake --build build-experiments --parallel
./build-experiments/easy_uds_fd_passing_probe
./build-experiments/easy_uds_io_uring_probe
./build-experiments/easy_uds_false_sharing_probe 100000000
./build-experiments/easy_uds_zero_copy_probe 16777216
```

`easy_uds_fd_passing_probe` transfers a `memfd_create()` descriptor with
`SCM_RIGHTS` over a Unix socketpair and validates the received contents.
`easy_uds_io_uring_probe` only calls `io_uring_setup`; it is a capability probe,
not an alternate reactor. The current production backend remains epoll until a
complete io_uring implementation demonstrates a measurable benefit.

`easy_uds_false_sharing_probe` compares two adjacent relaxed atomics with two
64-byte-aligned atomics. It is a hardware-sensitive diagnostic; it does not
justify padding production state unless the target workload reproduces the gap.

`easy_uds_zero_copy_probe` compares file-backed `sendfile()` and `splice()` with
a read/write copy over a Unix socketpair. It runs two passes: `raw` and
`framed` (20-byte header + 64 KiB payload per frame, mirroring the real wire
protocol). Caution: this probe's baseline is a plain `read`+`write()` copy
(~2.6–3 GiB/s on the dev host), not the library's gathered `sendmsg` path. An
in-library `sendfile` file-stream measured through the real non-blocking stream
path was a 4–7x regression against the callback path and the 0.6.4 file-source
API was **rejected**; zero-copy is deferred to the io_uring backend
(`IORING_OP_SENDFILE`). See `PERF_0.6.md`. The probe remains a documentation of
the naive transport result.

## ARM64 validation

The repository has no hosted ARM64 runner configured. On an ARM64 SBC or
self-hosted runner, run `scripts/arm64_smoke.sh` for the normal release build,
unit tests, and stress target, then extend the run at
1 KiB, 64 KiB, and 1 MiB payloads with connection counts 1/8/32/64, then repeat
the session and streaming benchmarks. Record p50/p95/p99, throughput, CPU,
context switches, and a shutdown/timeout soak before marking the ARM64 roadmap
items complete.

The script exits with status 2 on non-ARM64 hosts so an x86 build cannot be
mistaken for ARM coverage. Set `EASY_UDS_ALLOW_NON_ARM64=1` only for a local
dry build check.
