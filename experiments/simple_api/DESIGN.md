# Simple API promotion audit record

The prototype now has a production candidate in
`include/easy_uds/simple.hpp`. This record keeps the decisions and the
rejected alternatives with the experiment rather than hiding them in a
release note.

## Response/application error semantics

**Decision: A — `ResponseError` throw.** A transport/protocol/timeout/closed
failure remains `easy_uds::Error`. A completed RPC with a non-200 application
status throws `easy_uds::simple::ResponseError`, preserving `status()` and
`body()`.

`SimpleResponse` and an expected-like result were rejected for v1 because they
would force every beginner call to unwrap a second result model. The Core API
remains available when callers need a normal `Response` value or custom status
handling. The decision is implemented and covered by the simple experiment and
installed consumer.

## Proxy lifetime

The registration proxy is temporary-only. Its assignment operators are
ref-qualified with `&&`, so this is valid:

```cpp
server.on("/ping") = "pong";
```

while this is rejected at compile time:

```cpp
auto route = server.on("/ping");
route = "pong";
```

The proxy is not a route handle and has no unassigned destructor side effect.
This keeps the beginner syntax short without inventing a lifetime-bearing
registration object.

## Null strings and handler signatures

`std::string`, `std::string_view`, and `const char*` results are supported.
A null C string throws a deterministic handler exception with the message
`easy-uds simple handler returned a null C string`; it never relies on the
implementation-defined behavior of `std::string(nullptr)`.

The only accepted callable signatures are `()` and `(std::string_view)`. The
GCC/Clang probe checks key diagnostic phrases for invalid assignment, input,
return, and retained-proxy cases. Exact compiler output is intentionally not a
golden file.

## Core escape hatch and duplicate routes

`simple::Server::core()` returns the one underlying Core server. It does not
create another listener, reactor, or ownership domain. Simple and advanced
routes therefore share the same lifecycle and route registry. Duplicate route
registration is rejected exactly as Core registration is; Simple adds no
replace or magic semantics.

## A/B performance gate

Command:

```bash
cmake -S . -B build-simple -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_SIMPLE_EXPERIMENTS=ON \
  -DEASY_UDS_BUILD_EXAMPLES=OFF -DEASY_UDS_WARNINGS_AS_ERRORS=ON
cmake --build build-simple --parallel
bash scripts/simple_core_benchmark_median.sh build-simple 30000 5
```

WSL2, g++ 15.2, Release, 30,000 one-shot requests per row, five alternating
runs per load/API, and the median of each metric (each binary performs its own
1,000-request warmup). The order of Core and Simple is alternated for every
repeat. This is a host-specific promotion signal, not a portable performance
guarantee:

| Load | Core throughput | Simple throughput | Core p50/p99 | Simple p50/p99 | Core CPU-s/1M | Simple CPU-s/1M |
|---|---:|---:|---:|---:|---:|---:|
| c1 | 10.35k/s | 10.91k/s | 76.91/272.10 us | 69.17/270.29 us | 89.30 | 83.05 |
| c8 | 32.75k/s | 36.49k/s | 218.69/555.43 us | 202.36/490.40 us | 99.55 | 88.27 |
| c32 | 32.85k/s | 29.52k/s | 886.68/1832.91 us | 916.13/2163.85 us | 104.10 | 109.84 |

The allocation probe reports `10.5005` Core allocations/request and `10.5001`
Simple allocations/request at 30,000 requests. Median RSS was 4,584--4,848
KiB for Core and 4,680--4,716 KiB for Simple across the six load/API rows.
The c32 row is the widest scheduling-sensitive spread (about 10% throughput
and 18% p99 in this sample); c1/c8 are neutral-to-better. No extra
steady-state allocation or RSS growth is present. Keep this table as the
baseline and rerun it on a fixed host before changing the adapter.

## Promotion result

The production header is promoted with a deliberately narrow v1 surface:
`simple::Server`, `simple::Client`, temporary route assignment,
`ResponseError`, and `core()` escape hatch. No `simple::Session`, stream,
stats, options, FD, typed RPC, retry, reconnect, router, or protocol feature
is added. Those remain Core or future experiments.
