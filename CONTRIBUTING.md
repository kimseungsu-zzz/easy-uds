# Contributing

## Build

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

The developer preset enables warnings-as-errors.

## Before opening a pull request

- Keep the public API small and C++17-compatible.
- Add or update tests for behavior changes.
- Preserve binary-safe protocol handling.
- Avoid unbounded queues, detached threads, descriptor ownership ambiguity, and exception escape from worker threads.
- Run both Debug and Release tests when changing concurrency or protocol code.
- Run ASan/UBSan and TSan for memory, concurrency, or wire-format changes.
- Run the protocol fuzzer when changing header parsing or length validation.
- Update `docs/PROTOCOL.md` for any wire-format change and bump the protocol version when compatibility breaks.
- Update `CHANGELOG.md` for user-visible changes.
- When changing `README.md`, review and update `README.ko.md` in the same
  change so both entry points expose the same beginner-facing product surface.
- Keep the CMake project version and `src/user/cpp/core/easy_uds/version.hpp` in sync for releases.
- Treat pre-1.0 minor releases as potentially ABI-breaking and keep the configured `SOVERSION` policy intact.

## Formatting

The repository includes `.clang-format` and `.editorconfig`.

```bash
clang-format -i src/user/cpp/core/easy_uds/*.hpp src/user/cpp/simple/easy_uds/*.hpp src/system/**/*.cpp src/system/**/*.hpp examples/*.cpp tests/*.cpp
```
