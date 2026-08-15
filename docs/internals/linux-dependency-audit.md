# Linux dependency and backend boundary audit

This is the Phase 5 audit for easy-uds 0.7.1. It records every Linux/POSIX
dependency currently visible under `src/system` after the Phase 4 capability
extractions. The purpose is to make the 0.8 backend entry points explicit, not
to introduce a runtime abstraction or to pretend that a POSIX-shaped seam is a
portable contract.

The enforced direction remains:

```text
src/user/*  ->  src/system  ->  src/system/platform[/linux]
```

The system layer may call a concrete capability and may interpret a native
result for retry, timeout, stale-path, or public-error policy. It must not own
the Linux syscall that produces that result.

## Complete dependency classification

| Dependency or operation | Current owner | Phase 5 classification | 0.8 decision/blocker |
|---|---|---|---|
| `AF_UNIX`, `sockaddr_un`, `sys/socket.h`, `sys/un.h` | `platform/linux/endpoint.cpp`; value declaration in `platform/endpoint.hpp` | Linux implementation; POSIX-oriented temporary seam | Endpoint/address representation must be redesigned for the Windows transport model; `UnixEndpoint` is not frozen as a universal endpoint |
| `sys/types.h`, `ssize_t`, `iovec` | `platform/descriptor_passing.hpp`, `platform/socket_io.hpp`, Linux implementations | POSIX type support for concrete I/O seams | These types are not a final Windows backend contract; see the 0.8 buffer/handle decision |
| `sys/uio.h` | Descriptor passing and ordinary gathered-write capability | Linux/POSIX implementation detail | Keep out of public headers; replace or relocate only after the Windows transport shape is selected |
| `fcntl.h`, `O_CLOEXEC`, `O_NOFOLLOW`, `O_NONBLOCK`, `FD_CLOEXEC` | `platform/linux/socket_lifecycle.cpp`, `descriptor_passing.cpp`, `server_path.cpp` | Linux descriptor/path setup implementation | Preserve close-on-exec and symlink defenses; no generic `NativeHandle` abstraction in 0.7.1 |
| `unistd.h`, `close`, `shutdown`, `read`, `write`, `unlink`, `geteuid` | `platform/linux/socket_lifecycle.cpp`, `readiness.cpp`, `descriptor_passing.cpp`, `server_path.cpp` | Linux descriptor/wakeup/path implementation | Windows handle and wakeup ownership need a separate 0.8 design |
| `sys/stat.h`, `lstat`, `fstat`, `S_ISSOCK`, `S_ISREG`, `st_uid`, `st_dev`, `st_ino`, `st_nlink` | `platform/linux/server_path.cpp`; `dev_t`/`ino_t` retained in reactor state | Linux pathname security implementation plus POSIX temporary identity value | Do not generalize inode/UID semantics before Windows endpoint ownership is decided |
| `sys/file.h`, `flock` | `platform/linux/server_path.cpp` | Linux instance-lock implementation | Windows named-object/lock semantics remain an open 0.8 capability |
| `socket`, `connect`, `bind`, `listen`, `accept4` | `platform/linux/endpoint.cpp` | Linux implementation | Windows socket/pipe setup is a separate backend source set |
| `epoll_create1`, `epoll_ctl`, `epoll_wait`, `EPOLL*` | `platform/linux/readiness.cpp` | Linux readiness implementation | IOCP/completion semantics may require a different 0.8 readiness contract |
| `eventfd`, `write`, `read`, `EFD_*` wakeup operations | `platform/linux/readiness.cpp` | Linux wakeup implementation | Windows wakeup/completion mechanism remains open; no universal event-loop interface is frozen |
| `poll`, `pollfd`, `POLLIN/POLLOUT/POLLERR/POLLHUP/POLLNVAL` | `platform/linux/socket_wait.cpp` | Linux synchronous-wait implementation | This one-descriptor result is transport-only and is not the final Windows/IOCP wait contract |
| `SO_PEERCRED`, `getsockopt`, `struct ucred` | `platform/linux/peer_identity.cpp` | Linux peer-identity implementation | Public `PeerCredentials` currently exposes POSIX `pid_t`/`uid_t`/`gid_t`; this is a Windows public-header blocker |
| `SCM_RIGHTS`, `cmsghdr`, `CMSG_*`, `sendmsg`, `recvmsg`, `MSG_CTRUNC`, `MSG_CMSG_CLOEXEC` | `platform/linux/descriptor_passing.cpp`; POSIX `iovec` declaration in `platform/descriptor_passing.hpp` | Linux descriptor-passing implementation | Windows HANDLE/resource passing is a separate 0.8 API decision; no generic resource-passing contract is frozen |
| `send`, `recv`, ordinary `sendmsg`/`iovec`, `getsockopt(SO_ERROR)` | `platform/linux/socket_io.cpp`; raw result consumed by transport/reactor | Linux low-level I/O implementation; transport owns retry and meaning | `ssize_t`/`iovec` in `platform/socket_io.hpp` are temporary POSIX seams; keep or move them only after Windows transport shape is known |
| `close`, `shutdown`, `fcntl`, `FD_CLOEXEC`, `O_NONBLOCK`, `setsockopt` setup | `platform/linux/socket_lifecycle.cpp` | Linux descriptor lifecycle/setup implementation | `OwnedFd`/`BorrowedFd` remain POSIX public APIs in 0.7.x; do not rename to generic handles here |
| `open`, `fstat`, `flock`, `fchmod`, `lstat`, `geteuid`, `unlink`, pathname `chmod` | `platform/linux/server_path.cpp` | Linux server pathname/instance lifecycle implementation | Device/inode, effective UID, `O_NOFOLLOW`, and `flock` have no portable meaning yet; do not create `PortableInode` or a universal lock API |
| `<cerrno>`, `errno`, `EINTR`, `EAGAIN`, `EWOULDBLOCK`, `EINPROGRESS` | `platform/linux/*` reports native values; transport/reactor uses them for retry/control flow | Legitimate system semantic errno; not a syscall ownership violation | A future Windows mapping must preserve the semantic distinction without exposing Linux errno in the public API |
| `ETIMEDOUT`, `ECONNRESET`, `EPIPE`, `EBADF`, `ECONNABORTED`, `ENOENT`, `EADDRINUSE` | Transport/reactor/runtime | User-visible or lifecycle policy after a capability call | Keep native code in `Error::system_code()` where available; define Windows mappings only with the 0.8 backend |
| `uid_t` in `peer_identity` and public `PeerCredentials` | `platform/linux/peer_identity.cpp`; public request header | POSIX value/capability and a public portability blocker | Decide portable identity vs optional platform identity before Windows headers are supported |
| `dev_t`, `ino_t` in `reactor/core.hpp` and `server_path::Identity` | `server_path` capability supplies values; reactor state retains the pair for unlink TOCTOU checks | Engine security state backed by a POSIX temporary seam | Replace or hide the identity representation only after Windows pathname ownership semantics are designed |
| `ssize_t`, `iovec`, integer descriptor arguments in platform headers | `platform/descriptor_passing.hpp`, `platform/socket_io.hpp` | POSIX-oriented temporary seams, not common portable contracts | Retain, move under Linux, or replace with system-owned buffers in 0.8; do not add type erasure now |
| `socket`/descriptor integers in `reactor` and `transport` | Policy calls concrete `platform_linux`/capability functions; no direct raw syscall | Genuine engine policy using a platform capability | Build-time source selection is sufficient for 0.7.1; no `ITransport`/`IPlatform` virtual layer |

## Direct ownership check

The raw syscall inventory has no accidental implementation outside the intended
capabilities:

- `platform/linux/endpoint.cpp` owns pathname socket setup.
- `platform/linux/readiness.cpp` owns epoll/eventfd and wakeup I/O.
- `platform/linux/peer_identity.cpp` owns `SO_PEERCRED`.
- `platform/linux/descriptor_passing.cpp` owns ancillary data and descriptor
  materialization/cleanup.
- `platform/linux/socket_lifecycle.cpp` owns close/shutdown/fcntl/socket setup.
- `platform/linux/socket_io.cpp` owns byte/gathered I/O and connect completion
  queries.
- `platform/linux/socket_wait.cpp` owns one synchronous poll attempt.
- `platform/linux/server_path.cpp` owns pathname identity, secure lock-file
  operations, and pathname unlink/chmod.

`runtime`, `reactor`, and `transport` still contain `errno` checks, but those
are retry/control-flow or user-visible policy after a capability call. They do
not include Linux syscall headers or issue the corresponding raw syscall.
Protocol code has no Linux dependency. User C++ code has no platform include;
the C and Python directories remain reserved boundaries.

## POSIX seams that are intentionally not frozen

The following headers remain in `src/system/platform/` because they are
consumed by system policy, but their current shapes are explicitly temporary:

- `endpoint.hpp` exposes `sockaddr_un` and `UnixEndpoint`.
- `descriptor_passing.hpp` exposes `ssize_t`, `iovec`, and integer FDs.
- `socket_io.hpp` exposes `ssize_t`, `iovec`, and integer FDs.
- `server_path.hpp` exposes `uid_t`, `dev_t`, `ino_t`, and `nlink_t`.
- `peer_identity.hpp` is internal. The public POSIX `PeerCredentials` value is
  now in `peer_credentials.hpp` and is delivered only through the explicit
  `posix::RequestCapabilities` view; the common `Request` header no longer
  exposes POSIX credential types.
- `readiness.hpp` and `socket_wait.hpp` are concrete Linux-driven seams, not a
  promise that Windows will use epoll/poll-shaped readiness.

No generic `NativeHandle`, `ITransport`, `IPlatform`, `GenericIovec`, virtual
backend, or shared-pointer backend is introduced to hide these facts early.

## CMake backend assembly

`CMakeLists.txt` now separates `EASY_UDS_COMMON_SOURCES` from the selected
`EASY_UDS_PLATFORM_SOURCES` and `EASY_UDS_PLATFORM_HEADERS`:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # src/system/platform/linux/*.cpp
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # 0.8 entry point; not implemented by 0.7.1
endif()
```

The Linux list is the complete capability set above. The Windows branch fails
explicitly with a 0.8 design message rather than accidentally compiling Linux
sources. This is build-time selection only; the runtime remains concrete and
the public API/protocol are unchanged.

## `server_path` native-result decision

`unlink_socket_path()` and `chmod_socket_path()` retain their small raw `int`
return plus immediate caller-side `errno` capture. They are cold lifecycle
operations, and this matches the existing endpoint/socket I/O capability style
without introducing a generic result framework. The multi-state operations
(`inspect()` and `acquire_lock()`) continue to use small explicit result types
because they need identity/status fields in addition to native errors.

## Audit invariants

- protocol v2 is unchanged;
- public Core and Simple APIs are unchanged;
- no per-request conversion, allocation, virtual dispatch, or type erasure was
  added;
- architecture guard remains `user -> system -> platform`;
- Windows implementation, C ABI, Python binding, and generic handle APIs stay
  0.8 work.
