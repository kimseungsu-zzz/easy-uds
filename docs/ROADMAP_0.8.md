# 0.8 portability decisions and release handoff

0.7.1 froze the User/System/platform ownership map. The 0.8 release work uses
that map to provide a concrete Windows AF_UNIX backend without pretending that
POSIX capabilities are portable. This document records the decisions made for
the initial RC and the blockers intentionally deferred beyond it.

## Public-header blocker

`PeerCredentials` currently exposes `pid_t`, `uid_t`, and `gid_t` from
`<sys/types.h>`. That remains a POSIX-only public capability. The common
Request and umbrella headers no longer require this header on Windows, so the
Windows public surface is compilable without fabricating UID/GID values.

The RC decision is a separate POSIX extension/capability. Windows PID/SID/token
identity is not aliased to Linux credentials and remains a future capability.
`OwnedFd`/`BorrowedFd` remain POSIX descriptor APIs; no `NativeHandle` wrapper
was introduced.

The current `src/system/platform/descriptor_passing.hpp` also exposes POSIX
`ssize_t`, `iovec`, and integer descriptors. In 0.8, decide whether to retain
that Linux-oriented seam, move it under the Linux directory, or replace it with
a system-owned buffer description after the Windows transport shape is known.

The Phase 4F `src/system/platform/socket_io.hpp` seam likewise exposes POSIX
`ssize_t`/`iovec` for raw and gathered I/O. It is an internal Linux-oriented
boundary today; do not present it as a final Windows socket contract before the
0.8 transport model is selected.

Phase 4G adds `src/system/platform/socket_wait.hpp` for one synchronous wait
attempt. Its current `Interest`/result shape is useful for the Linux transport
seam, but it is not a promise that Windows will use `poll`-style waiting; an
IOCP or another completion model may require a different setup-time contract.

Phase 4H keeps server pathname identity and instance locking Linux-specific.
The current security model depends on POSIX device/inode, effective UID,
`O_NOFOLLOW`, and `flock` semantics. Windows endpoint ownership and stale-name
behavior remain an open design item rather than a `PortableInode` or universal
file-lock API.

Phase 5 completed the Linux dependency audit and makes backend assembly visible
in CMake: `EASY_UDS_COMMON_SOURCES` is combined with one concrete platform
capability source set at build time. The 0.8 Windows branch now contains an
AF_UNIX/Winsock implementation for endpoint, I/O, readiness, wakeup, and
pathname lifecycle. See
[`internals/linux-dependency-audit.md`](internals/linux-dependency-audit.md) for
the per-dependency classification and the list of POSIX-shaped seams that must
not be mistaken for final Windows contracts.

## 0.8 handoff priority

The following handoff items record the 0.8 validation boundary. The Windows
implementation is present, and the final release's external compiler and
runtime job passed; every later release must repeat the same gate.

### P0 — public-header blockers

- **Resolved for the initial RC:** `PeerCredentials` and POSIX request
  capabilities stay in the POSIX extension surface; common Windows headers do
  not include them.
- **Resolved for the initial RC:** `OwnedFd`/`BorrowedFd` remain explicitly
  POSIX descriptors. Windows resource passing is out of scope.

### P1 — engine/platform seam blockers

- **Selected for the initial RC:** Winsock `AF_UNIX` keeps the existing
  pathname byte-stream model through a concrete build-time endpoint seam.
- POSIX `ssize_t`/`iovec` headers remain internal temporary seams; the Windows
  implementation uses `WSABUF` internally and does not expose those types in
  common public headers.
- Windows pathname identity is conservative: only paths known to the backend
  are treated as sockets; Windows ACL/identity parity is deferred.

### P2 — backend architecture decisions

- The initial RC uses concrete `WSAPoll` readiness plus UDP wakeup. This is not
  frozen as the final IOCP architecture.
- Windows endpoint uses AF_UNIX; HANDLE/SOCKET passing is intentionally absent.
- Instance ownership uses a concrete Windows lock-file capability; full ACL and
  stale-name parity remains a future design item.

The 0.8 implementation order is P0 public surface, then P1 concrete seams,
then P2 backend capabilities. No item is solved by adding a runtime virtual
backend or a generic handle type before its semantics are known.

## Capability decisions still open after the initial RC

- Windows readiness currently uses a concrete `WSAPoll`/UDP-wakeup capability;
  it is not frozen as the final IOCP architecture.
- Windows resource passing and HANDLE ownership require a separate API design;
  `SCM_RIGHTS` is not a cross-platform contract.
- C and Python bindings must consume a stable user-facing boundary and must not
  reimplement transport, reactor, or protocol behavior.

The Linux and Windows capability implementations are evidence for these
decisions, not a frozen virtual-backend hierarchy.
