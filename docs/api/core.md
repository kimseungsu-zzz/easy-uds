# Core API

This page is the compact reference for the types used in the first example.
For advanced contracts, follow the focused pages in the [API index](README.md).

## `Server`

```cpp
easy_uds::Server server(socket_path, options);
server.on(route, handler);                    // exact fixed route
server.on_prefix(prefix, handler);            // longest matching prefix
server.on_serialized(route, handler);         // default FIFO domain
server.on_stream(route, stream_handler);      // half-duplex stream
server.run();                                 // blocks; single-use server
server.stop();                                // idempotent, thread-safe
```

`on()` handlers may run concurrently on the worker pool. A handler must protect
mutable captured state and must not retain a `RequestContext`. Registration may
be updated while `run()` is active. `run()` owns the reactor and returns after
`stop()` or an unrecoverable startup/shutdown error.

Use `RouteOptions` when a handler needs context or a named serialization domain:

```cpp
server.on(
    "/drive/velocity",
    easy_uds::RouteOptions{
        [](const easy_uds::Request& request,
           const easy_uds::RequestContext& context) {
            if (context.stop_requested()) {
                return easy_uds::Response{easy_uds::status_request_timeout,
                                          "expired"};
            }
            return drive(request.body);
        }}
        .serialize_in("drivetrain", easy_uds::QueuePolicy::latest_wins));
```

`fifo` is the default. `latest_wins` replaces only an older pending request
for the same concrete route; `reject_if_busy` returns `409` while the domain is
busy. Neither policy interrupts a handler that already started.

## `Client` and `Session`

```cpp
easy_uds::Client client(socket_path, client_options);
auto response = client.request("/echo", "hello");
easy_uds::Session session = client.session();
auto concurrent_response = session.request("/echo", "hello");
```

`Client::request()` is one connection per call and is safe to call concurrently
on one `Client`. `Session` is move-only and multiplexes fixed requests over one
persistent connection; concurrent `Session::request()` calls are safe as long
as the object is not concurrently moved or destroyed. After an I/O error,
timeout, or peer close, that Session is permanently broken; create a new one
explicitly rather than relying on retry or replay.

## `Request` and `Response`

`Request` contains the route, body, peer credentials, protocol request id, and
an optional move-only `OwnedFd`. The handler owns the received descriptor for
the duration of the call; duplicate it when retaining it after return. `Response`
contains a wire-transparent non-negative `Status` and a string body. The
`Response::ok(body)` helper covers the common success case without changing the
aggregate `Response{status, body}` form used for explicit statuses. The
`status_*` constants cover the common `200`, `404`, `408`, `409`, `500`, and
`503` outcomes; applications may return another non-negative status.

## Streaming

`on_stream()` and `request_stream()` use a dedicated connection. The request
body is pulled through a reusable buffer until the reader returns `0`; only
then does the response stream begin. The chunk view passed to a response
callback is temporary and must be consumed or copied before the callback
returns. Stream size and timeout limits come from `ServerOptions`/
`ClientOptions`; `io_timeout` still detects a stalled peer.

## Options that affect boundaries

| Option | Meaning |
|---|---|
| `max_message_size` | Maximum fixed route+body and response body size |
| `max_inflight_requests_per_connection` | Per-peer fixed-request admission cap |
| `max_inflight_request_bytes_per_connection` | Per-peer queued/executing byte cap |
| `max_output_bytes_per_connection` | Per-peer unsent response cap |
| `request_timeout` | Absolute fixed-request deadline; handler cancellation is cooperative |
| `io_timeout` | Maximum idle interval between successful I/O progress |
| `stats` | Opt-in cumulative counters; gauges remain observable by snapshot |

The complete option list and defaults live in [`options.hpp`](../../include/easy_uds/options.hpp).
