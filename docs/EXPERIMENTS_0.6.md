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
```

`easy_uds_fd_passing_probe` transfers a `memfd_create()` descriptor with
`SCM_RIGHTS` over a Unix socketpair and validates the received contents.
`easy_uds_io_uring_probe` only calls `io_uring_setup`; it is a capability probe,
not an alternate reactor. The current production backend remains epoll until a
complete io_uring implementation demonstrates a measurable benefit.

File-backed zero-copy (`sendfile`/`splice`) remains unimplemented because the
current callback-based `StreamReader` contract does not expose a file descriptor.
It should be evaluated as a separate file-source API experiment, not by changing
the existing stream callback semantics.
