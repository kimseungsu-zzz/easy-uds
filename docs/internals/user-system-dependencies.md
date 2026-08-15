# User/System dependency audit

This is the 0.7.1 Phase 2 inventory after the behavior-neutral relocation in
`36154fd`. It is an audit, not a refactor: protocol v2, runtime behavior, and
the hot path are unchanged. The purpose is to make the remaining dependency
direction explicit before extracting Linux capabilities.

## Target direction

```text
src/user/cpp  ───────►  src/system  ───────►  src/system/platform/linux
src/user/c    ───────►  src/system  (future C ABI)
src/user/py   ───────►  src/user/c or a stable C++ boundary (future binding)
```

The desired end state is that `src/system` does not depend on a language
binding layer, `src/system/platform/linux` does not include any `src/user/*`
header, and `src/user/*` does not include a platform backend. The current tree
is an intermediate state: the C++ runtime implementation still uses the
public C++ value and handler contracts directly. That coupling is recorded
below rather than hidden behind a premature virtual interface.

## Direct public-header edges

| System component | Direct public C++ headers | What the edge is used for | Classification |
|---|---|---|---|
| `system/core/error.cpp` | `error.hpp` | Defines `Error`, `ErrorCode`, and the error category | Public error contract implementation |
| `system/protocol/codec.hpp` | `error.hpp` | Reports malformed magic/version/type/flags as `Error` | Engine error contract; protocol validation |
| `system/transport/io.hpp` | `error.hpp`, `options.hpp`, `request.hpp` | Validates client/server limits and constructs peer credentials | Engine policy contract; platform capability value |
| `system/transport/transport.hpp` | `options.hpp`, `response.hpp`, `stream.hpp` | Frames fixed and streaming requests and returns `Status` | Protocol values plus public stream contract |
| `system/runtime/client.cpp` | `client.hpp` | Defines `Client` methods and one-shot response handling | Public API implementation |
| `system/runtime/session.cpp` | `session.hpp` | Defines `Session`, in-flight slots, reader loop, and stats snapshot | Public API implementation plus engine concurrency |
| `system/reactor/core.hpp` | `server.hpp` | Stores handlers/options and defines the server-side dispatch state | Mixed public API adapter and engine state |

`system/runtime/server.cpp` has no direct public-header include of its own; it
includes `reactor/core.hpp` and defines the `Server` member functions declared
by the transitive `server.hpp` include. It is therefore still a public API
implementation edge.

All other reactor translation units include `reactor/core.hpp` (directly or
through `common.hpp`, `parser.hpp`, or `stream_io.hpp`). Their public C++
dependency is consequently transitive, not absent.

## Requested symbol inventory

| Symbol | Current users | Classification | Long-term note |
|---|---|---|---|
| `Client` | `runtime/client.cpp` | Public API implementation | Keep concrete; split method glue from transport operations before moving files again |
| `Session` | `runtime/session.cpp` | Public API implementation plus engine concurrency | `SessionState` is the likely concrete engine boundary; no virtual client needed |
| `Server` | `runtime/server.cpp`, `reactor/core.hpp` | Public API implementation | Registration/lifecycle glue can eventually sit beside user/cpp while dispatch state stays system-owned |
| `Request` | reactor jobs, handlers, transport | Protocol/handler value | A request is both decoded wire data and the user handler input; an internal mirror would add conversion cost |
| `Response` | worker output, client/session readers | Protocol/handler value | Status/body are wire values; the aggregate is currently the zero-copy handler boundary |
| `RouteOptions` | handler registry and serialized dispatch | Engine scheduling contract exposed through public API | Domain/policy can later be translated once at registration, not per request |
| `ServerOptions` | `ServerState`, option validation | Engine configuration contract | Contains limits/deadlines/backpressure that directly size engine state |
| `ClientOptions` | one-shot client, `SessionState`, transport | Engine configuration contract | Used for deadlines, framing limits, stream limits, and optional stats |
| `PeerCredentials` | `Connection`, `Request`, `capture_peer_credentials` | Platform capability value | Produced by Linux `SO_PEERCRED`; extraction target for `platform/linux` |
| `OwnedFd` / `BorrowedFd` | request FD delivery and client FD passing | Platform capability and ownership contract | The wrapper is public; raw descriptor acquisition belongs below the future platform boundary |
| `RequestContext` | `RequestContextFactory`, contextual workers | Engine execution context exposed to handlers | Candidate for a small immutable adapter over internal arrival/deadline/stop state |
| `QueuePolicy` | `RouteScheduling`, serialized worker | Engine scheduling contract | FIFO/latest-wins/reject-if-busy are concrete policy values, not polymorphic queues |
| `Status` / status constants | protocol encode/decode and responses | Protocol value | Keep numeric wire status separate from handler convenience helpers |
| `Error` / `ErrorCode` | codec, I/O, workers, public callers | Engine error contract | Preserve semantic kind plus native system code without a new hierarchy |

`StatsMode`, `ServerStats`, and `SessionStats` are optional observability
contracts. They are read at the public boundary, while their counters and
accounting remain system-owned. They are intentionally not a transport
abstraction.

## `reactor/core.hpp` map

`reactor/core.hpp` is the highest-risk mixed-ownership file. Its current
contents can be divided without changing behavior as follows:

| Region | Examples in the file | Ownership classification |
|---|---|---|
| User-facing handler contracts | `Server::Handler`, `RouteOptions::SimpleHandler`, `RouteOptions::ContextHandler`, `Server::StreamHandler`, `Response`, `Request`, `RequestContext`, `QueuePolicy` | `src/user/cpp` API values and adapters |
| Engine scheduling/dispatch state | `HandlerEntry`, `RouteScheduling`, `HandlerRegistry`, `PendingJob`, `SerializedJob`, `SerializedDomainActivity`, `SerializedAdmission`, `ServerCounterState` | Genuine system engine contract |
| Connection/reactor state | `Connection`, `ReactorConnection`, `ParsePhase`, output queue, parser offsets, generation, read-pause flags | Genuine reactor/transport state |
| Server lifecycle/configuration | `ServerState`, worker/serialized queues, lifecycle mutexes, `ServerOptions` | Mixed: public options enter at the API edge; state is system-owned |
| Protocol values | `HeaderBytes`, request id, wire arguments, frame offsets | Protocol codec/dispatch value |
| Linux capability residue | raw `fd`, `close()` in `Connection::~Connection`, `dev_t`, `ino_t`, epoll/wakeup/lock descriptor fields, received FD queue | Future `system/platform/linux` extraction target |

The first minimal boundary is therefore not an `IServer`/`ITransport` class. It
is a concrete translation seam at registration and lifecycle boundaries:

1. public route/options calls create immutable internal registry entries;
2. reactor jobs carry concrete internal state plus the existing request/value;
3. platform helpers produce descriptor and peer-credential values;
4. public `Client`/`Session`/`Server` methods remain thin entry points.

That seam can be introduced incrementally with ordinary structs and functions.
No `virtual`, `shared_ptr` polymorphism, or type-erased request is justified by
this inventory.

## Component summary

| Component | Public C++ dependency? | Genuine engine contract? | Linux/platform capability? |
|---|---:|---:|---:|
| `system/core` | Error header only | Error classification | No direct syscall today |
| `system/protocol` | Error header only | Wire codec and validation | `arpa/inet.h` only; no UDS syscall |
| `system/runtime/client` | Yes: Client/Response/options/FD/stream | Request deadlines and one-shot lifecycle | Through transport helpers |
| `system/runtime/session` | Yes: Session/Response/options/stats | In-flight table, reader/waiter, send serialization | Through transport helpers |
| `system/runtime/server` | Yes, transitively through reactor core | Lifecycle and route registration | Direct Linux includes remain |
| `system/reactor` | Yes, transitively through `core.hpp` | Connections, parsing, workers, queues | Direct epoll/socket/eventfd/poll use |
| `system/transport` | Yes: options/request/response/stream/error | Exact I/O and framing | Direct socket/uio/unix/fcntl use |
| `system/platform/linux` | No code yet | Reserved capability boundary | Placeholder only |
| `user/cpp` | Owns public C++ headers | Public call/handler syntax | Must not include backend headers |
| `user/c`, `user/py` | No implementation yet | Future binding surfaces | Must depend downward only |

## Next phase, explicitly deferred

This audit does not move Linux syscalls, add Windows code, add C/Python APIs,
change protocol v2, optimize the hot path, or introduce a virtual transport.
The next implementation step should be a small concrete seam for the mixed
`reactor/core.hpp` regions, followed by an inventory-driven move of `epoll`,
`eventfd`, `AF_UNIX`, `sockaddr_un`, `accept4`, `SO_PEERCRED`, `SCM_RIGHTS`,
`chmod`/`unlink`, and `errno` into `system/platform/linux` where that reduces
coupling without adding a call or allocation to the hot path.
