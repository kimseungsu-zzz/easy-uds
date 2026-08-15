#pragma once

// Current concrete endpoint contract.  The value remains POSIX-oriented while
// Linux owns its syscall implementation in platform/linux/endpoint.cpp.  It is
// kept outside the Linux implementation directory so system policy code does
// not include a backend header directly.

#include <string>
#include <string_view>

#include <sys/socket.h>
#include <sys/un.h>

namespace easy_uds::detail::platform_linux {

using NativeSocket = int;

struct UnixEndpoint {
    sockaddr_un address{};
    socklen_t length = 0;
};

UnixEndpoint make_endpoint(std::string_view socket_path);

NativeSocket create_stream_socket() noexcept;
int connect_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept;
int bind_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept;
int listen_socket(NativeSocket socket, int backlog) noexcept;
NativeSocket accept_socket(NativeSocket listener) noexcept;

int unlink_socket(const char* socket_path) noexcept;
int chmod_socket(const char* socket_path, unsigned int permissions) noexcept;

} // namespace easy_uds::detail::platform_linux
