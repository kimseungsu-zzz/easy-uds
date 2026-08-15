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

Phase 4 begins the inventory-driven extraction with `endpoint.*`: pathname
`AF_UNIX`/`sockaddr_un` validation and socket lifecycle calls are now concrete
Linux capability functions. Phase 4B adds the current reactor readiness
contract and its concrete Linux `epoll`/`eventfd` implementation; this seam is
not frozen as the final cross-platform backend contract. Phase 4C adds the
concrete peer-identity capability, keeping `SO_PEERCRED`/`getsockopt` in Linux
code and leaving the public `PeerCredentials` value unchanged. Descriptor
passing is now isolated as the concrete Linux ancillary-data capability while
preserving first-successful-send-only attachment and fatal malformed/truncated
receive semantics. Error translation remains a later capability unit. `src/system`
owns the engine and must not depend on C or Python binding layers;
`src/system/platform/linux` must not include `src/user/*`; and `src/user` must
not depend on a platform implementation. Build-time backend selection is
preferred over a hot-path `ITransport` virtual abstraction.
