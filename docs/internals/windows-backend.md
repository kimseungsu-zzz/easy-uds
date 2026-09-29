# Windows backend implementation notes

## Decision

The Windows backend uses Winsock AF_UNIX on Windows 10 or newer. AF_UNIX
keeps the existing pathname endpoint and byte-stream framing model, so the
common protocol v2/v3, fixed RPC, multiplexed Session, serialization policies,
and Simple API share the same engine. Named Pipes were not selected because
they would require a second message/connection model.

## Capability ownership

`src/system/platform/windows/` contains concrete implementations for:

- endpoint creation, bind/listen/connect/accept;
- socket close, shutdown, and nonblocking setup;
- byte and gathered writes, receive, and connect error query;
- one-socket `WSAPoll` wait;
- reactor readiness and UDP wakeup signaling;
- endpoint pathname/instance lock lifecycle;
- POSIX ancillary descriptor passing (Windows uses a separate HANDLE API).

No runtime `ITransport`, `IPlatform`, `shared_ptr` backend, or type-erased I/O
object was introduced. CMake selects exactly one platform source set.

## Peer identity and HANDLE transfer

Winsock AF_UNIX provides a kernel-observed peer PID through
`SIO_AF_UNIX_GETPEERPID`. The backend captures it at accept time, then attempts
to read the process primary token SID. PID is available when the ioctl works;
SID can be absent when the process exits or access is denied. The
`RequestContext` authorization callback exposes both values, and the C/Python
context callbacks carry the peer identity snapshot. This is process identity; it does
not prove which thread issued a request if the connected socket was shared.

Windows supports one arbitrary HANDLE on a one-shot fixed request. The protocol
carries the source handle value; the server duplicates it from the
kernel-identified peer process with `DuplicateHandle` only after authorization.
The callback receives a borrowed handle and must duplicate it to retain it.
This is not `SCM_RIGHTS` and depends on Windows granting the server
`PROCESS_DUP_HANDLE` access to the peer. C ABI v1 and the optional Python
ctypes package cover fixed RPC, contextual handlers, Sessions, cooperative
cancellation, and this callback-scoped HANDLE view. Streaming remains a C++
API.

The HANDLE API transfers duplicable Win32 handles. It rejects pseudo handles
and does not transfer Winsock `SOCKET` values; socket transfer would need the
separate `WSADuplicateSocket` and `WSAPROTOCOL_INFO` contract. A process handle
with `PROCESS_DUP_HANDLE` access is captured at accept time when Windows grants
it. If access is denied, peer PID capture can still succeed while HANDLE
requests fail with status 400. Keep the source handle open until the response
arrives to avoid close-and-reuse races.

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

The 1.0 baseline passed the dedicated [GitHub Actions run
31919103353](https://github.com/kimseungsu-zzz/easy-uds/actions/runs/31919103353),
including static/shared package consumers. The 1.1 development sources were
built locally with Visual Studio 2022/MSVC; the Windows smoke passed with
peer-PID authorization and live HANDLE transfer coverage. Linux was also
built with GCC warnings-as-errors and its 11 CTest cases passed. These results
do not validate BSD native compilation; run the platform gates again on the
release commit.

The pathname capability is deliberately conservative when Windows does not
expose POSIX inode/type information: only paths recorded as bound by this
backend are classified as sockets. An unrelated existing file is never treated
as a stale socket. Full cross-process stale-name and ACL parity is deferred to
the Windows endpoint design phase rather than approximated with fake POSIX
identity values.
