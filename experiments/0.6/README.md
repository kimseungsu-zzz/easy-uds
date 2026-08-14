# easy-uds 0.6 experiments

This directory contains standalone capability and transport probes from the
0.6 performance phase. They are preserved as source evidence, are not linked
into the normal library target, and are never installed as public API.

Build them explicitly with:

```bash
cmake -S . -B build-experiments \
  -DEASY_UDS_BUILD_TESTS=OFF \
  -DEASY_UDS_BUILD_EXAMPLES=OFF \
  -DEASY_UDS_BUILD_EXPERIMENTS=ON
cmake --build build-experiments --parallel
```

The result matrix, command lines, and adoption/rejection rationale live in the
[0.6 experiment history](../../docs/history/experiments/0.6.md). The probes
must not be interpreted as a promise that io_uring, sendfile, or shared memory
is an alternate production transport.
