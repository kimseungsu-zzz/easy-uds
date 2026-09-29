# C API

The installed `easy_uds.h` header provides ABI version 1 with opaque server,
client, Session, and cancellation handles. It supports binary fixed requests
and contextual server handlers without exposing C++ types. Contextual handlers
receive request id, stop state, and available peer identity (POSIX PID/UID/GID
or Windows PID/SID); unavailable fields carry sentinel values. The
`stop_requested` integer is an initial snapshot; call
`is_stop_requested(opaque_context)` to poll the live state during the callback.
Build and install the CMake package, then include
`<easy_uds.h>` and link `easy_uds::easy_uds`.

Functions return zero on success. On failure, read `easy_uds_last_error()` and
`easy_uds_last_error_code()` on the same thread before making another C API
call. Returned response buffers belong to the caller and must be released with
`easy_uds_bytes_free()`. A handler response can optionally provide a release
callback; it is called after the body has been copied.

Handlers may run concurrently on the server worker pool. Keep `user_data` alive
until the server has stopped and `easy_uds_server_run()` has returned; protect
mutable callback state with the application's own synchronization.

Use `easy_uds_client_create_with_options()` to select protocol v3, then create
a persistent Session. `easy_uds_session_request_cancellable()` accepts a
shareable cancellation handle; another thread can call
`easy_uds_cancellation_cancel()`. The request remains in flight until its
normal response arrives. Idempotent one-shot retry is explicit through
`easy_uds_client_request_idempotent()`. On Windows, the C client can pass one
HANDLE with `easy_uds_client_request_handle()`; contextual callbacks receive a
borrowed `received_handle`. Streaming remains available through C++.
