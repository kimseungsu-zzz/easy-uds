#include "../endpoint.hpp"

#include <cstddef>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace easy_uds::detail::platform_linux {

UnixEndpoint make_endpoint(std::string_view socket_path) {
    UnixEndpoint endpoint{};
    endpoint.address.sun_family = AF_UNIX;

    if (socket_path.empty()) {
        throw std::invalid_argument("socket path must not be empty");
    }
    if (socket_path.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(
            "pathname socket path must not contain embedded NUL bytes");
    }
    if (socket_path.size() >= sizeof(endpoint.address.sun_path)) {
        throw std::invalid_argument("socket path is too long");
    }

    std::memcpy(endpoint.address.sun_path, socket_path.data(), socket_path.size());
    endpoint.address.sun_path[socket_path.size()] = '\0';
    endpoint.length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
    return endpoint;
}

NativeSocket create_stream_socket() noexcept {
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
}

int connect_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept {
    return ::connect(socket, reinterpret_cast<const sockaddr*>(&endpoint.address),
                     endpoint.length);
}

int bind_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept {
    return ::bind(socket, reinterpret_cast<const sockaddr*>(&endpoint.address),
                  endpoint.length);
}

int listen_socket(NativeSocket socket, int backlog) noexcept {
    return ::listen(socket, backlog);
}

NativeSocket accept_socket(NativeSocket listener) noexcept {
    return ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
}

} // namespace easy_uds::detail::platform_linux
