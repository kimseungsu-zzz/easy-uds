# Request context

`RequestContext` gives an advanced fixed-request handler read-only access to
request timing and cooperative-stop state. The original one-argument handler
remains the default and does not construct a context:

```cpp
server.on("/ping", [](const easy_uds::Request&) {
    return easy_uds::Response{200, "pong"};
});
```

Opt in by wrapping a two-argument handler in `RouteOptions`:

```cpp
server.on(
    "/motor/set",
    easy_uds::RouteOptions{
        [](const easy_uds::Request& request,
           const easy_uds::RequestContext& context) {
            if (context.stop_requested()) {
                return easy_uds::Response{408, "command no longer useful"};
            }
            apply_motor_command(request.body);
            return easy_uds::Response{200, "ok"};
        }});
```

The same `RouteOptions` form works with `on_prefix()` and
`on_serialized()`. Keeping advanced registration in an options object allows
later scheduling and queue-policy controls to extend that object without
adding another family of `on_*` methods.

## Observations

| Member | Meaning |
|---|---|
| `request_id()` | The protocol-v2 correlation id. It is `0` for a one-shot request and normally nonzero for a Session request. |
| `arrival_time()` | `steady_clock` time when the server observed the first byte of the request frame. It is not a client send timestamp. |
| `deadline()` | Absolute server request deadline, or an empty optional when `ServerOptions::request_timeout` is disabled. |
| `deadline_expired()` | Whether that absolute deadline has passed at the instant of the call. |
| `connection_closing()` | Whether the server has observed the connection becoming unusable or has started closing it. `false` is not a peer-liveness guarantee. |
| `server_stopping()` | Whether `Server::stop()` has started server shutdown. |
| `stop_requested()` | Convenience OR of connection closing, server stopping, and deadline expiry. |

The deadline is the same end-to-end server deadline used for queue admission,
handler execution, and response I/O. A handler is not forcibly interrupted
when it expires. The application decides where it is safe to poll
`stop_requested()` and return.

There is no protocol-level per-request cancellation frame in 1.0. A client
disconnect, Session failure, server shutdown, or elapsed deadline can make the
cooperative signal true. Blocking application operations must provide their
own interruption mechanism if they need immediate wake-up.

## Lifetime and thread safety

`RequestContext` is a non-owning view valid only for the duration of its
handler call. It is deliberately non-copyable and non-movable; do not retain a
pointer or reference after returning. `request_id()`, arrival, and deadline
are immutable. POSIX peer credentials are provided separately through the
capability view; they are not a member or accessor of `RequestContext` itself.
Connection and server state are lock-free atomic
observations and may change while the handler runs.

Contextual handlers run under the same worker, serialized-executor, exception,
and response rules as simple handlers. The context adds no allocation per
request. A simple handler pays only one predictable route-entry branch; the
fixed-request body remains unchanged; the platform-neutral `Request` layout is
documented in the 0.8 capability migration record.

## POSIX capabilities

On Linux, include `<easy_uds/posix.hpp>` and opt into the capability view:

```cpp
server.on("/inspect", easy_uds::RouteOptions{
    [](const easy_uds::Request&, const easy_uds::RequestContext& context) {
        const auto capabilities =
            easy_uds::posix::request_capabilities(context);
        const easy_uds::PeerCredentials peer = capabilities.peer_credentials();
        const easy_uds::BorrowedFd fd = capabilities.received_fd();
        // `peer.present` is false when the platform cannot provide identity.
        // `fd` is valid only during this callback.
        return easy_uds::Response{200, fd.valid() && peer.present ? "ok" : "none"};
    }});
```

`RequestCapabilities` is a copyable, pointer-sized non-owning view. Copying or
moving it never extends the `RequestContext` lifetime. A received descriptor is
owned by the internal request job; call `BorrowedFd::duplicate()` to retain an
independent `OwnedFd` beyond the callback. The common `Request` type therefore
contains only route, body, and request-id values and remains explicitly
move-only.
