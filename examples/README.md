# Examples

The examples are ordered from the smallest usable program to a production-like
resource composition. All route names use a leading `/` for consistency with
health checks and other URI-shaped tooling.

| Order | Example | Use it to learn |
|---:|---|---|
| 1 | [`server.cpp`](server.cpp) + [`client.cpp`](client.cpp) | one-shot fixed RPC only |
| 2 | [Getting started](../docs/getting-started/README.md) | build, run, and choose an API path |
| 3 | [`streaming_server.cpp`](streaming_server.cpp) + [`streaming_client.cpp`](streaming_client.cpp) | streaming only when the application needs it |
| 4 | [Robot HAL server](robot_hal_server.cpp) | real resource mutexes paired with named domains, queue policies, context, and stats |

The basic example intentionally uses plain `on()` and `Client::request()` plus
the small `Response::ok()` helper. The Robot HAL example is the advanced
composition: its drivetrain and arm mutexes
are separate because the corresponding named serialization domains are allowed
to run in parallel. `LatestWins` only replaces pending velocity commands, while
`RejectIfBusy` leaves the Session usable after a rejected calibration request.

For focused contracts, use the [API reference](../docs/api/README.md) rather
than copying internals from an example.
