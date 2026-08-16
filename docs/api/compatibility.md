# 1.0 compatibility contract

easy-uds 1.0 freezes the public C++ and protocol contract that was validated
through the 0.9 stabilization line. This page states the promises that are
safe to build on without turning the project into a larger compatibility
framework.

## What is stable

- The wire protocol remains version **2**: its 20-byte header, request IDs,
  fixed frames, streaming frames, status values, and malformed-frame handling
  are unchanged.
- `Server`, `Client`, `Session`, `Request`, `Response`, `RequestContext`,
  `Stream`, `Error`, route options, queue policies, statistics, and the Simple
  facade keep their 0.9 source-level behavior.
- `Request` and `Session` remain move-only. `OwnedFd` owns exactly one POSIX
  descriptor; `BorrowedFd` never closes one; `duplicate()` is explicit.
- A `Session` never reconnects or replays implicitly. A broken or moved-from
  object keeps its documented terminal state.
- Installed packages expose the same headers and the `easy_uds::easy_uds`
  CMake target. Package configuration reports the matching project version.

## Platform capability differences

Linux and Windows 10+ provide the common Core, Session, streaming, Simple, and
installed-package surfaces. POSIX-only peer credentials and descriptor passing
are intentionally absent from Windows common headers. Windows PID data is not
presented as Linux `PeerCredentials`, and Windows HANDLE/resource passing is
not a hidden substitute for `SCM_RIGHTS`.

Applications that need a POSIX capability should include
`<easy_uds/posix.hpp>` explicitly and keep the returned `BorrowedFd` inside the
handler lifetime. This is a capability boundary, not a portability promise for
all operating systems.

## Error and retry policy

Catch `easy_uds::Error` and branch on its stable `ErrorCode`; inspect
`system_code()` only when the native cause matters. Timeouts and connection
failures do not interrupt a running handler. Applications decide whether a
failed operation is safe to retry. The library never retries, reconnects, or
replays automatically.

## Compatibility scope

1.0 is a stable C++17 source/API and protocol line. It is not a promise that
binary objects can be exchanged between arbitrary standard libraries or
compiler ABIs. Patch releases may correct defects and documentation while
preserving the contracts above; breaking API or protocol work belongs to a
future major line.
