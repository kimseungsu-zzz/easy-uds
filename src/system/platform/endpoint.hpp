#pragma once

// Current concrete endpoint contract.  The value remains socket-oriented while
// the selected platform owns its syscall implementation.  It is kept outside
// the backend directories so system policy code does not include a backend
// header directly.

#include "native_socket.hpp"

#include <string>
#include <string_view>

#if defined(_WIN32)
#include <winsock2.h>
#include <afunix.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#endif

namespace easy_uds::detail::platform_linux {

using NativeSocket = platform_types::NativeSocket;

struct UnixEndpoint {
    sockaddr_un address{};
    int length = 0;
};

UnixEndpoint make_endpoint(std::string_view socket_path);

NativeSocket create_stream_socket() noexcept;
int connect_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept;
int bind_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept;
int listen_socket(NativeSocket socket, int backlog) noexcept;
NativeSocket accept_socket(NativeSocket listener) noexcept;

} // namespace easy_uds::detail::platform_linux

#if defined(_WIN32)
namespace easy_uds::detail::platform_windows {

using NativeSocket = platform_types::NativeSocket;

struct UnixEndpoint {
    sockaddr_un address{};
    int length = 0;
};

UnixEndpoint make_endpoint(std::string_view socket_path);

NativeSocket create_stream_socket() noexcept;
int connect_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept;
int bind_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept;
int listen_socket(NativeSocket socket, int backlog) noexcept;
NativeSocket accept_socket(NativeSocket listener) noexcept;

} // namespace easy_uds::detail::platform_windows
#endif

namespace easy_uds::detail {
#if defined(_WIN32)
namespace platform = platform_windows;
#else
namespace platform = platform_linux;
#endif
} // namespace easy_uds::detail
