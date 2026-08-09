# Changelog

All notable changes to this project are documented here.

## 0.3.0

- Reworked socket I/O around non-blocking descriptors plus `poll()` instead of relying on per-syscall socket timeouts.
- Added absolute server/client `request_timeout` deadlines to stop slow-drip peers from extending a request indefinitely.
- Added a wakeup pipe so `Server::stop()` no longer closes the listening descriptor from another thread while `run()` may still be polling it.
- Added a per-socket advisory instance lock (`<socket_path>.lock`) to serialize easy-uds startup and stale-socket cleanup.
- Added current-user ownership checks, inode revalidation, and a configurable stale-socket grace period before unlinking a refused socket.
- Added concurrent-stop/start stress coverage, slow-drip deadline coverage, client transaction-deadline coverage, and duplicate-server ownership tests.
- Factored protocol header validation into a shared internal codec and added an optional Clang/libFuzzer target.
- Changed pre-1.0 shared-library `SOVERSION` to major.minor so incompatible minor ABIs do not masquerade as the same ABI.
- Protocol version remains `1`; v0.3.0 is wire-compatible with v0.2.x.

## 0.2.0

- Replaced newline-delimited framing with a versioned binary protocol.
- Added binary-safe routes and bodies.
- Replaced detached per-connection threads with a fixed worker pool.
- Added configurable connection limits and server/client timeouts.
- Added configurable Unix socket permissions with a secure `0600` default.
- Added `Server::is_running()` and socket path accessors.
- Changed socket failures to preserve `errno` through `std::system_error`.
- Hardened stale-socket cleanup and owned-path unlinking against replacement races.
- Fixed descriptor lifetime ordering during concurrent shutdown.
- Added malformed-peer, binary payload, timeout, shutdown, stale-path, permission, and concurrency tests.
- Added static/shared CI, ASan/UBSan/TSan CI, CMake package-consumer CI, and CMake presets.
- Made tests/examples default to off when embedded as a subproject.

## 0.1.0

- Initial C++17 request/response API over Unix Domain Sockets.
- Route handlers, 1 MiB framing limit, basic concurrent handling, tests, examples, and CMake package export.
