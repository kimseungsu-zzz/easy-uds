# Platform support in 1.1

The backend is selected at build time. The common engine still owns
protocol, framing, deadlines, dispatch, worker/session policy, and public API
semantics; the selected platform owns endpoint, socket I/O, synchronous wait,
readiness, wakeup, and pathname lifecycle primitives.

| Platform | Transport | Core fixed RPC | Session | Simple | Identity and resource capabilities |
| --- | --- | --- | --- | --- | --- |
| Linux | AF_UNIX | supported and regression-tested | supported | supported | peer credentials and one-FD `SCM_RIGHTS` |
| FreeBSD, OpenBSD, NetBSD, DragonFly BSD | AF_UNIX/kqueue (new; native validation pending) | shared engine; validation pending | shared engine | shared API | uid/gid where available; one-FD `SCM_RIGHTS` |
| Windows 10+ | Winsock AF_UNIX | implemented; 1.1 development build and smoke verified locally | implemented through the common engine | implemented | kernel peer PID, best-effort process SID, one-HANDLE fixed request transfer |

The 1.0 Windows workflow covered fixed RPC, concurrent Session requests,
streaming, Simple `ResponseError`, repeated bind/run/stop lifecycle, and
installed-package Core/Simple consumers. Its static and shared library
validation passed in
[Actions run 31924819563](https://github.com/kimseungsu-zzz/easy-uds/actions/runs/31924819563).
For the 1.1 development changes, Visual Studio 2022/MSVC built the Debug
configuration locally and the Windows smoke passed, including peer-PID
authorization and a live Win32 event HANDLE transfer. WSL2/GCC also built the
Linux configuration with warnings as errors; all 11 CTest cases passed. A
Linux shared-library smoke exercised the C ABI and Python RPC, context,
Session, and cancellation paths.

The Windows implementation deliberately uses AF_UNIX rather than introducing a
Named Pipe-specific protocol or a runtime transport hierarchy. This preserves
protocol v2 and the existing request-id/session machinery while keeping the
backend choice concrete in CMake. The current Windows readiness implementation
uses a concrete `WSAPoll` registry and UDP wakeup socket; it is not a promise
that this is the final IOCP architecture.

POSIX descriptor APIs (`Client::request_fd`,
`easy_uds::posix::RequestCapabilities`, `OwnedFd`, and `BorrowedFd`) remain
POSIX-only. Windows provides its own `Client::request_handle` and
`easy_uds::windows::RequestCapabilities` API for one duplicable Win32 HANDLE
on a one-shot fixed request. The C ABI and optional Python package expose the
same callback-scoped received handle on Windows. HANDLE transfer depends on
the server obtaining `PROCESS_DUP_HANDLE` access to the connected peer; it
does not support pseudo handles or Winsock SOCKETs.

Native FreeBSD, OpenBSD, NetBSD, and DragonFly BSD builds remain unverified in
this environment. The historical 1.0 hosted Windows run does not validate
the later 1.1 changes; repeat the platform-specific gates on the release
commit.
