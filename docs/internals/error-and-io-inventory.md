# Error and low-level I/O inventory

This inventory records the concrete low-level boundary carried into the 0.8
candidate. It is intentionally a map, not a virtual backend design. Selected
platform capabilities report raw results, native errors, or a small semantic
result; the system/runtime layer decides when that becomes `easy_uds::Error`.

## Error and errno usage

| Location | Values/operation | Classification | Current owner of user-visible meaning |
|---|---|---|---|
| `system/reactor/parser.cpp` | `EINTR`, `EAGAIN`, `EWOULDBLOCK` | Read retry/control flow | Reactor; no `Error` for retry |
| `system/reactor/event_loop.cpp` | `EINTR`, `ECONNABORTED`, `EAGAIN`, `EWOULDBLOCK` | Accept/wait retry or connection control flow | Reactor; fatal results use `throw_system_error` |
| `system/reactor/output.cpp` | `EINTR`, `EAGAIN`, `EWOULDBLOCK` | Nonblocking output retry/control flow | Reactor; fatal results use `throw_system_error` |
| `system/reactor/stream_io.hpp` | `EINTR`, `EAGAIN`, `EWOULDBLOCK`, `ECONNRESET` | Stream retry or peer-close mapping | Transport/reactor via `throw_system_error` |
| `system/transport/io.hpp` | `ETIMEDOUT`, `EBADF`, `EPIPE`, `ECONNRESET` | Deadline, invalid descriptor, closed peer | `throw_system_error` maps to public `Error` and preserves native code |
| `system/transport/io.hpp` | `EINPROGRESS`, `EAGAIN`, `EWOULDBLOCK`, `EINTR`, `SO_ERROR` | Nonblocking connect completion | Transport; only final failure becomes `Error` |
| `system/platform/linux/socket_wait.cpp` | `poll`, `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP`, `POLLNVAL` | One synchronous wait attempt | Returns a small native/semantic result; transport owns retry/deadline mapping |
| `system/platform/linux/server_path.cpp` | `open`, `fstat`, `flock`, `fchmod`, `lstat`, `geteuid`, pathname `unlink`/`chmod` | Server lock and pathname identity/security capability | Returns identity, lock status, and native errors; runtime owns stale/busy policy |
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
header for deadline/retry policy and framing. Raw socket lifecycle, byte
syscalls, and one synchronous wait attempt now live in concrete platform
capabilities. The current
responsibilities are:

| Responsibility | Operations | Phase decision |
|---|---|---|
| Descriptor lifecycle | `FileDescriptor`, close/shutdown | `platform/socket_lifecycle` concrete seam; public OwnedFd remains separate |
| Nonblocking configuration | `fcntl(F_GETFL/F_SETFL)`, `setsockopt(SO_NOSIGPIPE)` | `platform/socket_lifecycle`; raw errno mapped by transport |
| Wait-for-I/O | timeout calculation, `EINTR` retry, timeout/error mapping | `platform/socket_wait` performs one poll attempt; this is not the reactor readiness contract |
| Basic byte I/O | `send`, `recv`, exact loops, peer-close handling | `platform/socket_io` reports raw results; retry/closed semantics stay above |
| Gathered write | non-FD `sendmsg` with `iovec` | `platform/socket_io`; remains independent from descriptor ancillary capability |
| Descriptor-bearing write | `descriptor_passing::send_iovecs` | Extracted Linux capability; first-successful-send-only attachment retained |
| Connect completion | nonblocking `connect`, `SO_ERROR` query | Separate setup-time capability; final errno maps above the syscall boundary |

The normal gathered-write path remains separate from descriptor-bearing
`sendmsg`; no generic `Iovec`, virtual I/O backend, or type erasure is added.

`socket_lifecycle.hpp` owns internal descriptor close/shutdown and setup flags;
`socket_io.hpp` owns raw `send`/`recv`/ordinary `sendmsg` and the `SO_ERROR`
query. `socket_wait.hpp` owns the small one-descriptor wait result while its
Linux implementation owns `poll` and `POLL*` translation. All three seams
return native results only. `io.hpp` retains retry, deadline, peer-closed, and
public `Error` semantics above those calls; synchronous wait is intentionally
not merged with the multi-connection reactor readiness contract.

## Server pathname lifecycle

`src/system/platform/server_path.hpp` is a concrete Linux-oriented seam for
filesystem state associated with a server socket pathname. It owns the raw
`lstat`, effective-UID, lock-file (`O_NOFOLLOW`/`fstat`/`flock`/`fchmod`),
pathname `unlink`, and socket-path `chmod` operations. Its identity value keeps
the device/inode pair, entry kind, owner, and link count needed by the security
checks.

`system/runtime/server.cpp` still owns the logical policy: whether a path is
stale or busy, when the grace period expires, which identity comparisons must
precede removal, and how native failures become `Error` or a diagnostic
exception. The before/current/after identity checks and lock-file ownership,
regular-file, single-link, `O_NOFOLLOW`, and exclusive-lock invariants are not
weakened by the extraction.

## Descriptor seam status

`src/system/platform/linux/descriptor_passing.cpp` no longer includes a public
`easy_uds` header. Its receive API returns bytes, an optional descriptor, a
small ancillary semantic result, and native `errno` when close-on-exec setup
fails. `reactor/parser.cpp` is the first layer that converts those results into
the existing runtime exception/error behavior.

`src/system/platform/descriptor_passing.hpp` currently exposes POSIX
`ssize_t`, `iovec`, and integer descriptors. This remains a temporary
Linux/POSIX seam, not a final cross-platform contract. The 0.8 portability decision is recorded in
[`ROADMAP_0.8.md`](../ROADMAP_0.8.md): retain its shape, move it under Linux,
or replace it with a system-owned buffer description only after the Windows
transport model is known.
