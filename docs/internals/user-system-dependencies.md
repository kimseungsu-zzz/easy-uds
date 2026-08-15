# User/System dependency audit and concrete seam

This is the 0.7.1 Phase 2 inventory after the behavior-neutral relocation in
`36154fd`. It is an audit, not a refactor: protocol v2, runtime behavior, and
the hot path are unchanged. The purpose is to make the remaining dependency
direction explicit before extracting Linux capabilities.

Phase 3 now establishes the first concrete seam without introducing a virtual
backend or an internal Request/Response mirror. Public Client and Session
method glue lives under `src/user/cpp/core/`; one-shot/session engine state and
operations remain concrete functions under `src/system/runtime/`. Server route
registration glue is also user-owned, while registration-time translation into
immutable `HandlerEntry`/`RouteScheduling` entries is system-owned. No request
path conversion is performed.

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
| `system/transport/io.hpp` | `error.hpp`, `options.hpp`, `request.hpp` | Validates client/server limits and performs exact I/O around endpoint capability calls | Engine policy contract; platform capability value |
| `system/transport/transport.hpp` | `options.hpp`, `response.hpp`, `stream.hpp` | Frames fixed and streaming requests and returns `Status` | Protocol values plus public stream contract |
| `user/cpp/core/client.cpp` | `client.hpp` | Thin `Client` method glue | Public API implementation |
| `system/runtime/client_engine.*` | client/options/response/stream contracts | One-shot connect, framing, and response operations | Concrete engine implementation |
| `user/cpp/core/session.cpp` | `session.hpp` | Thin `Session` method glue and value-boundary access | Public API implementation |
| `system/runtime/session_engine.*` | session/options/response contracts | In-flight shards, reader, waiter, and shutdown state | Concrete engine concurrency |
| `user/cpp/core/server_api.cpp` | `server.hpp` | Route registration member-function glue | Public API implementation |
| `system/runtime/server_registration.*` | server/route contracts | One-time immutable handler/scheduling translation | Concrete engine registration |
| `system/reactor/core.hpp` | `server.hpp` | Stores handlers/options and defines the server-side dispatch state | Mixed public API adapter and engine state |

`system/runtime/server.cpp` still owns the Linux-heavy Server constructor,
lifecycle, run/stop, and stats implementation. It is intentionally the next
cold-path seam; moving it is deferred until the capability inventory is ready.

All other reactor translation units include `reactor/core.hpp` (directly or
through `common.hpp`, `parser.hpp`, or `stream_io.hpp`). Their public C++
dependency is consequently transitive, not absent.

## Requested symbol inventory

| Symbol | Current users | Classification | Long-term note |
|---|---|---|---|
| `Client` | `user/cpp/core/client.cpp`, `runtime/client_engine.*` | Public API glue plus concrete engine | Keep concrete; no virtual client needed |
| `Session` | `user/cpp/core/session.cpp`, `runtime/session_engine.*` | Public API glue plus engine concurrency | `SessionState` is the concrete engine boundary; no mirror or virtual client needed |
| `Server` | `user/cpp/core/server_api.cpp`, `runtime/server.cpp` | Registration glue plus lifecycle implementation | Route translation is complete; lifecycle remains system-owned until Linux extraction |
| `Request` | reactor jobs, handlers, transport | Protocol/handler value | A request is both decoded wire data and the user handler input; an internal mirror would add conversion cost |
| `Response` | worker output, client/session readers | Protocol/handler value | Status/body are wire values; the aggregate is currently the zero-copy handler boundary |
| `RouteOptions` | `user/cpp/core/server_api.cpp` then handler registry | Engine scheduling contract exposed through public API | Domain/policy is translated once at registration, not per request |
| `ServerOptions` | `ServerState`, option validation | Engine configuration contract | Contains limits/deadlines/backpressure that directly size engine state |
| `ClientOptions` | one-shot client, `SessionState`, transport | Engine configuration contract | Used for deadlines, framing limits, stream limits, and optional stats |
| `PeerCredentials` | `Connection`, `Request`, `peer_identity::Identity` conversion | Platform capability value | Produced by the peer-identity capability; public value semantics remain unchanged |
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
| Linux capability residue | raw `fd`, `close()` in `Connection::~Connection`, `dev_t`, `ino_t`, lock descriptor fields, received FD queue | Future `system/platform/linux` extraction target |

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
| `system/protocol` | Error header only | Wire codec and validation | Dependency-free big-endian codec; no UDS syscall |
| `system/runtime/client_engine` | Yes: Response/options/FD/stream values | Request deadlines and one-shot lifecycle | Through transport helpers |
| `system/runtime/session_engine` | Yes: Response/options/stats values | In-flight table, reader/waiter, send serialization | Through transport helpers |
| `user/cpp/core/client/session` | Owns public member glue | Calls concrete engine functions and state | No platform backend dependency |
| `system/runtime/server` | Yes, transitively through reactor core | Lifecycle and route registration | Direct Linux includes remain |
| `system/reactor` | Yes, transitively through `core.hpp` | Connections, parsing, workers, queues, readiness policy | Uses the platform-neutral readiness contract; no direct epoll/eventfd headers |
| `system/transport` | Yes: options/request/response/stream/error | Exact I/O and framing | Direct socket/uio/unix/fcntl use |
| `system/platform/linux/endpoint.*` | No public C++ API | Pathname endpoint and socket lifecycle capability | Linux `AF_UNIX`/socket syscalls |
| `system/platform/linux/readiness.cpp` | No public C++ API | Readiness registration/wait and wakeup signal/consume | Linux `epoll`/`eventfd` syscalls |
| `system/platform/linux/peer_identity.cpp` | No public C++ API | Connected-peer identity capture | Linux `SO_PEERCRED`/`getsockopt` |
| `system/platform/linux/descriptor_passing.cpp` | No public C++ API | Descriptor-bearing send/receive and ancillary validation | Linux `SCM_RIGHTS`/`recvmsg`/`sendmsg` |
| `user/cpp` | Owns public C++ headers | Public call/handler syntax | Must not include backend headers |
| `user/c`, `user/py` | No implementation yet | Future binding surfaces | Must depend downward only |

## Protocol portability decision

The protocol codec previously used `arpa/inet.h` only for `htonl`/`ntohl`.
Protocol v2 now uses four-byte explicit big-endian reads and writes in the
header codec itself. This removes a network-header dependency without changing
the 20-byte wire format or adding a call on the hot path. The
`easy_uds.protocol_golden` CTest compares a representative encoded header
byte-for-byte and decodes it again; the existing fuzz target continues to
exercise malformed headers.

## Next phase, explicitly deferred

This phase does not add Windows code, C/Python APIs, change protocol v2,
optimize the hot path, or introduce a virtual transport. The concrete
user/system seam, endpoint/socket capability, readiness/wakeup capability,
peer-identity capability, and descriptor-passing capability are now in place.
The next implementation step is an inventory-driven move of error translation.
These capabilities should continue to land as small concrete units where that
reduces coupling without adding a call or allocation to the hot path.
