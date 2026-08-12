# 0.6 experimental probes

These targets deliberately do not change the `easy_uds` public API or default
epoll backend. They answer whether Linux/UDS capabilities are available before
we consider a production design.

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

`easy_uds_zero_copy_probe` compares file-backed `sendfile()` with a read/write
copy over a Unix socketpair. This is a transport experiment only; the existing
callback-based `StreamReader` contract remains unchanged. A production file-source
API would need separate framing, size limits, deadlines, and fallback behavior.

## ARM64 validation

The repository has no hosted ARM64 runner configured. On an ARM64 SBC or
self-hosted runner, run the normal release build plus the stress target at
1 KiB, 64 KiB, and 1 MiB payloads with connection counts 1/8/32/64, then repeat
the session and streaming benchmarks. Record p50/p95/p99, throughput, CPU,
context switches, and a shutdown/timeout soak before marking the ARM64 roadmap
items complete.
