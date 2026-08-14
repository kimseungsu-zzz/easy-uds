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
| `peer()` | The same captured `SO_PEERCRED` snapshot as `Request::peer`. |
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

There is no protocol-level per-request cancellation frame in 0.7. A client
disconnect, Session failure, server shutdown, or elapsed deadline can make the
cooperative signal true. Blocking application operations must provide their
own interruption mechanism if they need immediate wake-up.

## Lifetime and thread safety

`RequestContext` is a non-owning view valid only for the duration of its
handler call. It is deliberately non-copyable and non-movable; do not retain a
pointer or reference after returning. `request_id()`, `peer()`, arrival, and
deadline are immutable. Connection and server state are lock-free atomic
observations and may change while the handler runs.

Contextual handlers run under the same worker, serialized-executor, exception,
and response rules as simple handlers. The context adds no allocation per
request. A simple handler pays only one predictable route-entry branch; the
fixed-request body and `Request` layout remain unchanged.
