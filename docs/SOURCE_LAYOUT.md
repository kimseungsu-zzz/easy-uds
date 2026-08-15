# Source layout

The 0.7.1 foundation establishes a behavior-neutral ownership map for the
0.8 work. It does not add an abstraction layer, change protocol v2, or move a
runtime decision onto a virtual interface.

```text
src/
├── system/
│   ├── core/                 shared engine state and error implementation
│   ├── protocol/             protocol-v2 codec boundary
│   ├── runtime/              client, session, and server runtime
│   ├── reactor/              epoll dispatch, parsing, workers, and streams
│   ├── transport/            exact I/O and client framing helpers
│   └── platform/linux/       reserved Linux dependency boundary
└── user/
    ├── cpp/
    │   ├── core/             installed Core C++ headers
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
the concrete seams to use before any Linux syscall extraction.

The first relocation deliberately leaves Linux calls in their existing
implementation files and keeps the current C++ runtime behavior intact. The
next inventory phase will classify `epoll`,
`eventfd`, `AF_UNIX`, `sockaddr_un`, `accept4`, `SO_PEERCRED`, `SCM_RIGHTS`,
`chmod`/`unlink`, and `errno`, then move only the necessary pieces under
`src/system/platform/linux/`. `src/system` owns the engine and must not depend
on C or Python binding layers; `src/system/platform/linux` must not include
`src/user/*`; and `src/user` must not depend on a platform implementation.
Build-time backend selection is preferred over a hot-path `ITransport` virtual
abstraction.
