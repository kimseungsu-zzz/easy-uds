## easy-uds

A small, dependency-free C++17 request/response API built on Unix Domain Sockets.

### Features
- Server/Client abstraction with `easy_uds::Server` and `easy_uds::Client`
- Automatic socket cleanup
- C++17 compatibility
- Cross-platform (Linux/macOS/Windows 10)

### Build
```sh
mkdir build && cd build
cmake ..
make
ctest
```