# Windows backend implementation notes

## Decision

The 0.8 initial backend uses Winsock AF_UNIX on Windows 10 or newer. AF_UNIX
keeps the existing pathname endpoint and byte-stream framing model, so the
common protocol v2, fixed RPC, multiplexed Session, serialization policies, and
Simple API remain unchanged. Named Pipes were not selected because they would
require a second message/connection model before the cross-platform public
contract is stable.

## Capability ownership

`src/system/platform/windows/` contains concrete implementations for:

- endpoint creation, bind/listen/connect/accept;
- socket close, shutdown, and nonblocking setup;
- byte and gathered writes, receive, and connect error query;
- one-socket `WSAPoll` wait;
- reactor readiness and UDP wakeup signaling;
- endpoint pathname/instance lock lifecycle;
- unsupported descriptor passing and peer identity results.

No runtime `ITransport`, `IPlatform`, `shared_ptr` backend, or type-erased I/O
object was introduced. CMake selects exactly one platform source set.

## Unsupported capabilities

Windows does not expose a fabricated `PeerCredentials` value or a generic
native-handle wrapper. `SCM_RIGHTS` is rejected on the Windows backend and
`request_fd` is absent from Windows common headers. SID/token identity,
`WSADuplicateSocket`, HANDLE passing, C/Python bindings, and typed RPC remain
future capability decisions.

## Winsock error boundary

`socket_common.cpp` captures `WSAGetLastError()` immediately and translates
the error families used by endpoint, readiness, wait, and byte-I/O operations
to the existing errno-based transport boundary. In particular,
`WSAEWOULDBLOCK` becomes `EAGAIN`, setup/connect failures preserve address and
network distinctions (`EADDRINUSE`, `EADDRNOTAVAIL`, `ENETUNREACH`, and
`EHOSTUNREACH`), and closed-peer cases (`WSAECONNRESET`, `WSAESHUTDOWN`) map to
the existing reset/pipe semantics. Unknown Winsock values become `EIO` rather
than leaking a raw 100xx code into `Error::system_code()`.

## Validation boundary

The current development environment has no MSVC. Linux
build/tests/ASan/UBSan/TSan/fuzz/stress remain the local regression gate.
Windows source compilation and runtime behavior for this candidate passed in
the dedicated [GitHub Actions run
31916904359](https://github.com/kimseungsu-zzz/easy-uds/actions/runs/31916904359),
including static/shared package consumers; every later candidate must repeat
that hosted validation.

The pathname capability is deliberately conservative when Windows does not
expose POSIX inode/type information: only paths recorded as bound by this
backend are classified as sockets. An unrelated existing file is never treated
as a stale socket. Full cross-process stale-name and ACL parity is deferred to
the Windows endpoint design phase rather than approximated with fake POSIX
identity values.
