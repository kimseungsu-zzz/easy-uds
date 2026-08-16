# Platform support in 1.0

The 1.0 backend is selected at build time. The common engine still owns
protocol, framing, deadlines, dispatch, worker/session policy, and public API
semantics; the selected platform owns endpoint, socket I/O, synchronous wait,
readiness, wakeup, and pathname lifecycle primitives.

| Platform | Transport | Core fixed RPC | Session | Simple | POSIX capabilities |
| --- | --- | --- | --- | --- | --- |
| Linux | AF_UNIX | supported and regression-tested | supported | supported | peer credentials and one-FD `SCM_RIGHTS` |
| Windows 10+ | Winsock AF_UNIX | implemented and hosted-validated | implemented through the common engine | implemented | unavailable in 1.0; no fake FD/SID API |

The Windows release smoke covers fixed RPC, concurrent Session requests, streaming,
Simple `ResponseError`, repeated bind/run/stop lifecycle, and installed-package
Core/Simple consumers. Static and shared library variants are built in the
Windows workflow. The final 1.0 validation passed that full matrix in
[Actions run 31924489686](https://github.com/kimseungsu-zzz/easy-uds/actions/runs/31924489686).

The Windows implementation deliberately uses AF_UNIX rather than introducing a
Named Pipe-specific protocol or a runtime transport hierarchy. This preserves
protocol v2 and the existing request-id/session machinery while keeping the
backend choice concrete in CMake. The current Windows readiness implementation
uses a concrete `WSAPoll` registry and UDP wakeup socket; it is not a promise
that this is the final IOCP architecture.

Windows resource passing is out of scope for 1.0: `Client::request_fd`,
`easy_uds::posix::RequestCapabilities`, `OwnedFd`, `BorrowedFd`, and Linux
`PeerCredentials` remain POSIX-only surfaces. The common installed umbrella
headers do not include those headers on Windows.

The repository environment used for this development does not contain MSVC,
so no local Windows runtime result is claimed. The `windows-core` GitHub
Actions job builds the static/shared-capable library, runs the Windows smoke
test, installs the package, and compiles the installed consumer; its passing
result is recorded above. A future release must repeat that gate on its own
commit.
