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

## Validation boundary

The current development environment has no MSVC or Windows runner. Linux
build/tests/ASan/UBSan/TSan/fuzz/stress remain the regression gate. Windows
source compilation and runtime behavior are claimed only from the dedicated
GitHub Actions job after it executes; until then this is an explicit external
validation blocker, not a passing test result.

The pathname capability is deliberately conservative when Windows does not
expose POSIX inode/type information: only paths recorded as bound by this
backend are classified as sockets. An unrelated existing file is never treated
as a stale socket. Full cross-process stale-name and ACL parity is deferred to
the Windows endpoint design phase rather than approximated with fake POSIX
identity values.
