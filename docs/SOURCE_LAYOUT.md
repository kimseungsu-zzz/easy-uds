# Source layout

The 0.7.1 foundation establishes a behavior-neutral ownership map for the
0.8 work. It does not add an abstraction layer, change protocol v2, or move a
runtime decision onto a virtual interface.

```text
src/
├── system/
│   ├── core/                 shared engine state and error implementation
│   ├── protocol/             protocol-v2 codec boundary
│   ├── runtime/              concrete engine functions and server lifecycle
│   ├── reactor/              readiness dispatch, parsing, workers, and streams
│   ├── transport/            exact I/O and client framing helpers
│   └── platform/linux/       endpoint, readiness, identity, and FD capabilities
└── user/
    ├── cpp/
    │   ├── core/             installed Core C++ headers and public method glue
    │   └── simple/           installed Simple C++ header
    ├── c/                    reserved C ABI boundary
    └── py/                   reserved Python binding boundary
```

The installed include path remains `include/easy_uds/` for consumers. During
the build it is sourced from `src/user/cpp/core/easy_uds/` and
`src/user/cpp/simple/easy_uds/`, so public C++ ownership is visible without
changing the package surface.

The actual dependency inventory is maintained in
[`internals/user-system-dependencies.md`](internals/user-system-dependencies.md).
It records the current intentional `system → public C++ contract` edges and
the concrete seams used before any Linux syscall extraction. Client and Session
glue now calls concrete runtime engine functions, and route options are
translated once during registration into immutable internal entries.

Phase 4 begins the inventory-driven extraction with the endpoint contract and
Linux `endpoint.cpp`: pathname
`AF_UNIX`/`sockaddr_un` validation and socket lifecycle calls are now concrete
Linux capability functions. Phase 4B adds the current reactor readiness
contract and its concrete Linux `epoll`/`eventfd` implementation; this seam is
not frozen as the final cross-platform backend contract. Phase 4C adds the
concrete peer-identity capability, keeping `SO_PEERCRED`/`getsockopt` in Linux
code and leaving the public `PeerCredentials` value unchanged. Descriptor
passing is now isolated as the concrete Linux ancillary-data capability while
preserving first-successful-send-only attachment and fatal malformed/truncated
receive semantics. Phase 4E removes the public `Error` dependency from that
backend and lets the reactor map its raw/native results. The remaining generic
low-level error translation is still an inventory-driven follow-up. `src/system`
owns the engine and must not depend on C or Python binding layers;
`src/system/platform/linux` must not include `src/user/*`; and `src/user` must
not depend on a platform implementation. Build-time backend selection is
preferred over a hot-path `ITransport` virtual abstraction.

Phase 4D/4E keeps descriptor passing in the concrete Linux capability while
moving native ancillary results upward before public error construction. The
current `platform/descriptor_passing.hpp` seam intentionally exposes POSIX
`ssize_t`/`iovec` and is not a final Windows transport contract. The errno and
`io.hpp` responsibility inventory is maintained in
[`internals/error-and-io-inventory.md`](internals/error-and-io-inventory.md).

Phase 4F moves socket descriptor lifecycle, raw byte I/O, and connect-error
queries behind concrete system platform seams. The POSIX endpoint value
contract now lives in `src/system/platform/endpoint.hpp`; Linux syscall
implementations remain in `src/system/platform/linux/`.

Phase 4G adds `platform/socket_wait.hpp` with a concrete Linux implementation
for one synchronous `poll` attempt. Transport retains deadline calculation,
`EINTR` retry, timeout conversion, and public error mapping; this wait seam is
not the reactor readiness contract.

Phase 4H adds `platform/server_path.hpp` and its Linux implementation for
server pathname identity and instance-lock primitives. The capability preserves
`O_NOFOLLOW`, ownership/type/link checks, `flock`, and before/current/after
device/inode validation; runtime retains the stale/busy decision and public
error policy.

Phase 5 records the complete Linux/POSIX dependency inventory in
[`internals/linux-dependency-audit.md`](internals/linux-dependency-audit.md).
`CMakeLists.txt` now assembles common engine sources separately from the
selected `src/system/platform/linux/` source set. The Windows branch is an
explicit 0.8 entry point and still fails as unsupported in 0.7.1. The current
`endpoint.hpp`, `descriptor_passing.hpp`, `socket_io.hpp`, `server_path.hpp`,
and readiness/wait seams remain concrete POSIX/Linux contracts, not frozen
cross-platform interfaces.
