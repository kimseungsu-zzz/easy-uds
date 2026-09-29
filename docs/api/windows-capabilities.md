# Windows peer identity and HANDLE passing

On Windows, `RequestContext::peer_process_id()` reports the process ID captured
from the connected AF_UNIX peer. `peer_sid()` contains the process primary
token SID when the process remains accessible and token lookup succeeds. Treat
an empty SID as unavailable and fail closed when the application requires a
specific account.

To pass one owned or otherwise valid client process HANDLE, use a one-shot
`Client::request_handle()` call. The source remains owned by the client; keep
it open until the response arrives. Windows AF_UNIX and the kernel peer PID are
required by this feature.

```cpp
#include <easy_uds/client.hpp>
#include <easy_uds/windows.hpp>

easy_uds::Client client{R"(C:\Temp\service.sock)"};
const auto response = client.request_handle(
    "/inspect", easy_uds::windows::BorrowedHandle{file_handle}, "metadata");
```

Register a contextual handler to inspect the received capability:

```cpp
#include <easy_uds/windows.hpp>

server.on("/inspect", easy_uds::RouteOptions{
    [](const easy_uds::Request& request,
       const easy_uds::RequestContext& context) {
        if (context.peer_sid() != "S-1-5-21-...") {
            return easy_uds::Response{403, "Forbidden"};
        }
        const auto caps = easy_uds::windows::request_capabilities(context);
        const auto borrowed = caps.received_handle();
        if (!borrowed.valid()) {
            return easy_uds::Response{400, "Missing HANDLE"};
        }
        // Duplicate explicitly if the HANDLE must outlive this callback.
        auto retained = borrowed.duplicate();
        return easy_uds::Response{200, request.body};
    }});
```

The server duplicates the numeric HANDLE from the connected peer's process
table only after `ServerOptions::authorize_request` permits the fixed request.
The contextual handler receives a callback-scoped borrowed HANDLE; use
`duplicate()` to retain a separately owned copy. The sender must keep the
source HANDLE open through the response to avoid a close-and-reuse race.
`DuplicateHandle` preserves the source access rights. The server should still
apply its own authorization policy before using the object.

C `easy_uds_request_context::received_handle` and Python
`RequestContext.received_handle` expose the same callback-scoped HANDLE value.
The C++ `OwnedHandle` wrapper is the preferred way to retain ownership.

This API passes duplicable Win32 HANDLEs. It rejects pseudo handles; Winsock
`SOCKET` values use a different duplication API and are not supported by this
method. This is distinct from POSIX `SCM_RIGHTS`, which remains available
through `<easy_uds/posix.hpp>` on POSIX platforms.
