# 0.8 portability decisions

0.7.1 keeps the Linux/POSIX public contract explicit while extracting concrete
Linux capabilities behind the engine. The following items are deliberately
deferred to 0.8 design work; this file records blockers rather than promising a
particular backend shape.

## Public-header blocker

`PeerCredentials` currently exposes `pid_t`, `uid_t`, and `gid_t` from
`<sys/types.h>`. That is correct for the Linux-only 0.7 line, but these POSIX
types make the current public header unsuitable for a Windows build.

Before adding a Windows backend, decide whether peer identity becomes a
portable value, a platform-specific optional capability, or a separate
platform header. Do not silently rename it to `NativeHandle` or widen
`OwnedFd`/`BorrowedFd`; those remain POSIX descriptor APIs in 0.7.x.

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

Phase 5 completes the Linux dependency audit and makes backend assembly visible
in CMake: `EASY_UDS_COMMON_SOURCES` is combined with one concrete platform
capability source set at build time. The 0.8 Windows branch now contains an
AF_UNIX/Winsock implementation for endpoint, I/O, readiness, wakeup, and
pathname lifecycle. See
[`internals/linux-dependency-audit.md`](internals/linux-dependency-audit.md) for
the per-dependency classification and the list of POSIX-shaped seams that must
not be mistaken for final Windows contracts.

## 0.8 handoff priority

The 0.7.1 architecture is frozen with these handoff items, in order. The
Windows backend implementation has started, but the external Windows compiler
and runtime gate is still a release blocker until it runs.

### P0 — public-header blockers

- Decide how `PeerCredentials` can represent `pid_t`/`uid_t`/`gid_t` without
  making the public header POSIX-only.
- Decide whether `OwnedFd`/`BorrowedFd` remain explicitly POSIX descriptors or
  gain a separate Windows resource API. Do not rename or generalize them in
  0.7.x.

### P1 — engine/platform seam blockers

- Replace or retain the `sockaddr_un`/`UnixEndpoint` value seam after choosing
  the Windows endpoint model.
- Decide what replaces POSIX `ssize_t`/`iovec` seams for raw and gathered I/O.
- Decide how `dev_t`/`ino_t` pathname identity and TOCTOU ownership checks map
  to a Windows endpoint lifecycle.

### P2 — backend architecture decisions

- Choose readiness versus an IOCP/completion model; the current readiness and
  synchronous-wait contracts are not frozen as Windows interfaces.
- Choose Windows endpoint and resource/HANDLE-passing capabilities, if any.
- Define Windows instance ownership/locking and stale-name semantics.

The 0.8 implementation order is P0 public surface, then P1 concrete seams,
then P2 backend capabilities. No item is solved by adding a runtime virtual
backend or a generic handle type before its semantics are known.

## Capability decisions still open

- Windows readiness currently uses a concrete `WSAPoll`/UDP-wakeup capability;
  it is not frozen as the final IOCP architecture.
- Windows resource passing and HANDLE ownership require a separate API design;
  `SCM_RIGHTS` is not a cross-platform contract.
- C and Python bindings must consume a stable user-facing boundary and must not
  reimplement transport, reactor, or protocol behavior.

The Linux and Windows capability implementations are evidence for these
decisions, not a frozen virtual-backend hierarchy.
