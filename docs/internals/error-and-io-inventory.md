# Error and low-level I/O inventory

This inventory records the 0.7.1 Phase 4E boundary. It is intentionally a
concrete map, not a virtual backend design. Linux capabilities report raw
results, `errno`, or a small semantic result; the system/runtime layer decides
when that becomes `easy_uds::Error`.

## Error and errno usage

| Location | Values/operation | Classification | Current owner of user-visible meaning |
|---|---|---|---|
| `system/reactor/parser.cpp` | `EINTR`, `EAGAIN`, `EWOULDBLOCK` | Read retry/control flow | Reactor; no `Error` for retry |
| `system/reactor/event_loop.cpp` | `EINTR`, `ECONNABORTED`, `EAGAIN`, `EWOULDBLOCK` | Accept/wait retry or connection control flow | Reactor; fatal results use `throw_system_error` |
| `system/reactor/output.cpp` | `EINTR`, `EAGAIN`, `EWOULDBLOCK` | Nonblocking output retry/control flow | Reactor; fatal results use `throw_system_error` |
| `system/reactor/stream_io.hpp` | `EINTR`, `EAGAIN`, `EWOULDBLOCK`, `ECONNRESET` | Stream retry or peer-close mapping | Transport/reactor via `throw_system_error` |
| `system/transport/io.hpp` | `ETIMEDOUT`, `EBADF`, `EPIPE`, `ECONNRESET` | Deadline, invalid descriptor, closed peer | `throw_system_error` maps to public `Error` and preserves native code |
| `system/transport/io.hpp` | `EINPROGRESS`, `EAGAIN`, `EWOULDBLOCK`, `EINTR`, `getsockopt(SO_ERROR)` | Nonblocking connect completion | Transport; only final failure becomes `Error` |
| `system/runtime/server.cpp` | `ENOENT`, `EWOULDBLOCK`, `EAGAIN`, `EADDRINUSE` | Stale socket/instance lock lifecycle | Server lifecycle via `throw_system_error` or explicit busy `Error` |
| `system/platform/linux/readiness.cpp` | `EINTR`, `EINVAL`, raw syscall result | Linux readiness/wakeup capability | Reactor decides retry/translation |
| `system/platform/linux/descriptor_passing.cpp` | `MSG_CTRUNC`, malformed ancillary, native `fcntl` result | Descriptor capability semantic/native result | Parser maps semantic rejection; `throw_system_error` maps native setup failure |
| `system/core/error.cpp` | `std::generic_category()`, `classify_system_error()` | Public semantic classification | Sole public `Error` category implementation |

`EINTR`, `EAGAIN`, and `EWOULDBLOCK` remain control-flow values. They are not
wrapped in an `Error` merely because they are observable through `errno`.
`ETIMEDOUT`, closed-peer errors, and fatal syscall failures retain the existing
`ErrorCode` and `Error::system_code()` behavior.

## `io.hpp` portability hotspots

`src/system/transport/io.hpp` remains a deliberately mixed concrete utility
header. The current responsibilities are:

| Responsibility | Operations | Phase decision |
|---|---|---|
| Descriptor lifecycle | `fcntl`, `close`, RAII `FileDescriptor` | Existing system utility; future capability candidate |
| Nonblocking configuration | `fcntl(F_GETFL/F_SETFL)`, `setsockopt(SO_NOSIGPIPE)` | Existing setup path; no hot-path abstraction added |
| Wait-for-I/O | `poll`, `EINTR`, timeout conversion | Existing deadline utility; future readiness integration candidate |
| Basic byte I/O | `send`, `recv`, exact loops, peer-close handling | Keep concrete until a measured portability seam is justified |
| Gathered write | non-FD `sendmsg` with `iovec` | Keep with transport; do not couple ordinary writes to ancillary capability |
| Descriptor-bearing write | `descriptor_passing::send_iovecs` | Extracted Linux capability; first-successful-send-only attachment retained |
| Connect completion | nonblocking `connect`, `getsockopt(SO_ERROR)` | Keep in transport setup; final errno maps above the syscall boundary |

The normal gathered-write path remains separate from descriptor-bearing
`sendmsg`; no generic `Iovec`, virtual I/O backend, or type erasure is added.

## Descriptor seam status

`src/system/platform/linux/descriptor_passing.cpp` no longer includes a public
`easy_uds` header. Its receive API returns bytes, an optional descriptor, a
small ancillary semantic result, and native `errno` when close-on-exec setup
fails. `reactor/parser.cpp` is the first layer that converts those results into
the existing runtime exception/error behavior.

`src/system/platform/descriptor_passing.hpp` currently exposes POSIX
`ssize_t`, `iovec`, and integer descriptors. This is a 0.7.1 Linux seam, not a
final cross-platform contract. The 0.8 portability decision is recorded in
[`ROADMAP_0.8.md`](../ROADMAP_0.8.md): retain its shape, move it under Linux,
or replace it with a system-owned buffer description only after the Windows
transport model is known.
