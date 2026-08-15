# 0.8 Request capability delivery record

This record accompanies the portable `Request`/POSIX capability change.  It is
not a new benchmark target: the existing 0.6.4 regression gates remain the
decision gate for the hot path.

## Object footprint

The values below were measured with GCC 15, C++17, x86_64 Linux, and the same
release headers used by the unit test build.  The `before` row is a layout
probe reproducing the 0.7.1 `Request` fields and job fields; the `after` row is
the actual 0.8 capability implementation.

| object | before | after | change | reason |
|---|---:|---:|---:|---|
| `Request` | 88 B | 72 B | -16 B | POSIX peer and owning FD removed; explicit move-only value remains route/body/id |
| `PendingJob` | 192 B | 216 B | +24 B | job-local descriptor owner and peer snapshot are carried with the job |
| `SerializedJob` | 176 B | 200 B | +24 B | same owner/snapshot storage covers serialized, reject, and latest-wins paths |
| `RequestContext` | 40 B | 48 B | +8 B | private pointer-sized bridge to the callback-scoped capability storage |

The capability storage itself contains one internal move-only descriptor owner
and one value peer-identity snapshot.  It has no `shared_ptr`, side map, heap
sidecar, type erasure, or public POSIX wrapper.  The peer snapshot is copied
when a job is created, so it cannot dangle when its connection is closed.

## Allocation and ownership invariant

No allocation is added to a warmed Session request without an FD.  The normal
fixed-request path still moves the existing route/body strings and job values;
`RequestCapabilities` is only a pointer-sized non-owning view created for a
contextual handler.  A handler pays for a descriptor duplication only when it
explicitly calls `BorrowedFd::duplicate()`.

The current setup-inclusive allocation probe reports 12 allocations over
20,000 warmed Session requests (0.0006/request on this run). That counter also
includes allocator/runtime fluctuation around the benchmark harness; the
dedicated steady-state gate remains effectively zero allocations per request,
as recorded in `PERF_0.7.md`.

The descriptor owner chain is:

```text
recvmsg result
  -> descriptor_owner (immediate RAII adoption)
  -> ReactorConnection / RequestCapabilityStorage
  -> PendingJob or SerializedJob (move)
  -> handler-scoped BorrowedFd view
  -> job destruction (exactly one close)
```

Missing routes, `reject_if_busy`, `latest_wins` replacement, queue expiry,
handler exceptions, connection shutdown, and malformed ancillary data all
destroy the same internal owner.  Retention is explicit and independent:
`BorrowedFd::duplicate()` creates a new `OwnedFd` while leaving the job owner
and the caller's original descriptor untouched.

## Gate

The implementation is accepted only if the existing fixed 10-repeat
performance policy remains inside the 0.6.4 gates, the warmed no-FD Session
allocation probe remains zero allocations/request, and the FD leak,
sanitizer, TSan, package-consumer, Simple API, and architecture tests remain
green.
