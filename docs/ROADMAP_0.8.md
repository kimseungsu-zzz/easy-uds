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

## Capability decisions still open

- Windows readiness may use a completion model rather than the current Linux
  reactor readiness seam.
- Windows resource passing and HANDLE ownership require a separate API design;
  `SCM_RIGHTS` is not a cross-platform contract.
- C and Python bindings must consume a stable user-facing boundary and must not
  reimplement transport, reactor, or protocol behavior.

The Linux capability extractions in `src/system/platform/linux/` are evidence
for these decisions, not a frozen 0.8 virtual-backend hierarchy.
