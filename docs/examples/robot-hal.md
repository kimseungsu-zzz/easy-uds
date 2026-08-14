# Robot HAL example

`examples/robot_hal_server.cpp` is a small, complete server-side composition
of the 0.7 API. It simulates a drivetrain and arm HAL, so it does not require
robot hardware, but keeps the same boundaries a real driver can use:

```text
/health                        watchdog/readiness (plain on())
/diagnostics                   best-effort Server::stats() + HAL state
/drive/velocity                drivetrain domain + LatestWins
/arm/position                  arm domain + contextual FIFO
/drive/calibrate               drivetrain domain + RejectIfBusy
```

Build and run it from a Release tree:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_EXAMPLES=ON -DEASY_UDS_WARNINGS_AS_ERRORS=ON
cmake --build build --target easy_uds_robot_hal_server_example
./build/easy_uds_robot_hal_server_example /tmp/easy-uds-robot.sock
```

The default socket path is `/tmp/easy-uds-robot.sock`. The existing client
example can be adapted to call the routes, or a Session can multiplex calls:

```cpp
easy_uds::Client client("/tmp/easy-uds-robot.sock");
auto session = client.session();

const auto health = session.request("/health");
const auto velocity = session.request("/drive/velocity", "0.25");
const auto arm = session.request("/arm/position", "home");
const auto report = session.request("/diagnostics");
```

For velocity control, callers can issue a new `/drive/velocity` command without
waiting for an older queued command to finish: `LatestWins` replaces only the
older pending request for that concrete route and returns `409` to that old
caller. It never interrupts a handler that has already started. Calibration
uses `RejectIfBusy`, so a request arriving while the drivetrain domain is busy
gets `409` and the Session remains usable.

The example treats `RequestContext::stop_requested()` as cooperative input.
A real HAL should poll it around interruptible hardware operations and return
without starting work that is already past its deadline. It should not retain
the context after the handler returns.

`Server::stats()` is deliberately a snapshot, not a metrics exporter. The
example exposes a compact diagnostics RPC for local probes; production systems
can translate the same fields into their existing metrics/logging pipeline.
Keep `/health` short and stable, and use `/diagnostics` for operator-facing
detail rather than making a readiness probe parse an evolving stats payload.
