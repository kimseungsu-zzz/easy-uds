# Simple API prototype (experimental)

This directory retains the syntax and diagnostics experiment that preceded the
promotion. The shim is deliberately outside the installed include tree and is
not enabled by the normal build; the promoted header is now
`include/easy_uds/simple.hpp`.

Build and run the prototype explicitly:

```bash
cmake -S . -B build-simple \
  -DCMAKE_BUILD_TYPE=Release \
  -DEASY_UDS_BUILD_SIMPLE_EXPERIMENTS=ON \
  -DEASY_UDS_WARNINGS_AS_ERRORS=ON
cmake --build build-simple --parallel
ctest --test-dir build-simple -L simple --output-on-failure
```

The experiment probes only these forms:

```cpp
server.on("/ping") = "pong";
server.on("/echo") = [](std::string_view body) {
    return std::string(body);
};
server.on("/version") = [] { return "0.7-simple-experiment"; };
```

It adapts each assignment directly to the existing Core `Server::on()` and
`Response::ok()` path. No new router, protocol, retry, reconnect, streaming,
serialization, or runtime type registry is introduced. The promotion decision
and final error/lifetime policy are recorded in `DESIGN.md`.

The test also checks duplicate-route rejection and an explicit `core()`
escape hatch. Proxy lifetime is intentionally narrow: a `Route` must not be
retained after its owning `Server` is destroyed, and an unassigned proxy has
no registration side effect.

See [`docs/design/simple-api.md`](../../docs/design/simple-api.md) for the
promotion criteria and the rejected scope.
