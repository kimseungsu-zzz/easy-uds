# Compatibility and platform contract

Version 1.1.0 preserves the established C++ Core API and protocol v2 defaults
while adding opt-in capabilities. This page describes the 1.1.0 contract; check
the changelog and release notes for the version installed by an application.

## Source and wire compatibility

- Protocol v2 remains the default and keeps its 20-byte header, request IDs,
  fixed frames, streaming frames, and status values.
- Protocol v3 is opt-in through `ClientOptions::protocol_version`; it adds
  cooperative cancellation for persistent Session requests. A v2 peer rejects
  v3 frames, so both ends must select v3.
- Existing C++ request, response, ownership, route, Session, and Simple API
  behavior remains available. `Request` and `Session` remain move-only.
- POSIX `OwnedFd`/`BorrowedFd` and `Client::request_fd()` remain explicit
  descriptor APIs. Windows uses a separate one-shot `Client::request_handle()`
  API for duplicable Win32 HANDLE values.
- C ABI v1 is exposed through `easy_uds.h`. The optional standard-library-only
  Python package wraps that ABI; streaming remains a C++ API.
- `request_idempotent()` and the matching C/Python methods retry only when the
  caller explicitly opts in. Use them only for operations safe to repeat.
  Ordinary requests never retry or replay automatically.

## Platform capabilities

Linux is production-supported. Windows 10+ provides AF_UNIX, kernel peer PID,
best-effort process SID, and one-HANDLE fixed requests when the server can open
the peer with `PROCESS_DUP_HANDLE`. POSIX peer credentials and `SCM_RIGHTS`
remain POSIX-only. BSD backends are included in CMake, with native validation
still pending.

POSIX callers should include `<easy_uds/posix.hpp>` and keep borrowed
capabilities inside the handler lifetime. Windows callers should include
`<easy_uds/windows.hpp>` and duplicate a received borrowed HANDLE if they need
to retain it beyond the callback.

## Errors and retry policy

Catch `easy_uds::Error` and branch on its stable `ErrorCode`; inspect
`system_code()` when the native cause matters. Timeouts and connection
failures do not interrupt a running handler. A retry can repeat work if the
server completed the operation but the response was lost, so automatic retry
is intentionally absent. `request_idempotent()` is an explicit caller
assertion that repeating that route is safe.

## Binary compatibility

The C++ API is a C++17 source contract. This project does not promise that
binary objects can be exchanged between arbitrary standard libraries or
compiler ABIs. The C ABI is versioned separately; check
`easy_uds_c_abi_version()` before relying on a particular ABI generation.
