# Public headers and source layout

The original one-include API remains the recommended starting point:

```cpp
#include <easy_uds/easy_uds.hpp>
```

`easy_uds.hpp` is now a small umbrella rather than the file that owns every
declaration. Larger applications and reusable libraries may include only the
feature headers they expose or use:

```cpp
#include <easy_uds/client.hpp>
#include <easy_uds/error.hpp>
```

## Public header map

| Header | Responsibility |
|---|---|
| `easy_uds.hpp` | Compatibility umbrella containing the complete public API |
| `client.hpp` | One-shot `Client` API and Session factory |
| `session.hpp` | Persistent Session, state snapshots, fixed and streamed calls |
| `server.hpp` | Server lifecycle and route registration |
| `request.hpp` | `Request` and `PeerCredentials` |
| `response.hpp` | `Status`, status constants, and `Response` |
| `stream.hpp` | `StreamReader` and `StreamResponse` |
| `options.hpp` | `ClientOptions`, `ServerOptions`, and default size constants |
| `error.hpp` | `Error`, `ErrorCode`, and error category access |
| `fd.hpp` | `BorrowedFd`, `OwnedFd`, and ownership helpers |
| `version.hpp` | Library and wire-protocol version constants |

Every header is self-contained under C++17 and is installed by the CMake
package. A direct include does not require `easy_uds.hpp` to appear first.
Including the umbrella remains source-compatible with 0.6.

The split changes neither object layout nor symbol names. It does not change
the protocol-v2 wire format. It only gives declarations stable ownership and
prevents unrelated public declarations from being parsed when a narrow header
is sufficient.

## Internal source map

Internal files are grouped by runtime responsibility:

```text
src/client/       one-shot Client, persistent Session, shared stream transport
src/server/       Server lifecycle, routing, socket ownership, startup/shutdown
src/reactor/      epoll parser, dispatch, flow/output, streams, worker executors
src/protocol/     protocol-v2 codec boundary
src/detail/       shared descriptor, deadline, socket, and exact-I/O utilities
```

Files below `src/` are implementation details. Applications must not include
them or rely on their names; only `include/easy_uds/` is installed and covered
by the public compatibility policy.

The grouping deliberately avoids one class per tiny file. Client and Session
are separate because they have different lifetime and concurrency models;
shared framing lives in one transport unit so the fixed-request hot helpers
can remain inline across those translation units. Server and shared I/O remain
cohesive files while they are below the project's split threshold.
