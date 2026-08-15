#include "../endpoint.hpp"
#include "../socket_lifecycle.hpp"
#include "socket_common.hpp"

#if defined(_WIN32)
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace easy_uds::detail::platform_windows {

UnixEndpoint make_endpoint(std::string_view socket_path) {
    UnixEndpoint endpoint{};
    endpoint.address.sun_family = AF_UNIX;
    if (socket_path.empty()) {
        throw std::invalid_argument("socket path must not be empty");
    }
    if (socket_path.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("pathname socket path must not contain embedded NUL bytes");
    }
    if (socket_path.size() >= sizeof(endpoint.address.sun_path)) {
        throw std::invalid_argument("socket path is too long for Windows AF_UNIX");
    }
    std::memcpy(endpoint.address.sun_path, socket_path.data(), socket_path.size());
    endpoint.address.sun_path[socket_path.size()] = '\0';
    endpoint.length = static_cast<int>(offsetof(sockaddr_un, sun_path) +
                                       socket_path.size() + 1);
    return endpoint;
}

NativeSocket create_stream_socket() noexcept {
    if (!ensure_winsock()) {
        return platform_types::invalid_socket;
    }
    const SOCKET socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (socket == INVALID_SOCKET) {
        last_wsa_error();
        return platform_types::invalid_socket;
    }
    return from_socket(socket);
}

int connect_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept {
    const int result = ::connect(to_socket(socket),
                                 reinterpret_cast<const sockaddr*>(&endpoint.address),
                                 endpoint.length);
    if (result != 0) {
        last_wsa_error();
    }
    return result;
}

int bind_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept {
    const int result = ::bind(to_socket(socket),
                              reinterpret_cast<const sockaddr*>(&endpoint.address),
                              endpoint.length);
    if (result != 0) {
        last_wsa_error();
    } else {
        remember_bound_path(endpoint.address.sun_path);
    }
    return result;
}

int listen_socket(NativeSocket socket, int backlog) noexcept {
    const int result = ::listen(to_socket(socket), backlog);
    if (result != 0) {
        last_wsa_error();
    }
    return result;
}

NativeSocket accept_socket(NativeSocket listener) noexcept {
    const SOCKET accepted = ::accept(to_socket(listener), nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
        last_wsa_error();
        return platform_types::invalid_socket;
    }
    const NativeSocket result = from_socket(accepted);
    const auto setup = socket_lifecycle::set_nonblocking(result);
    if (!setup.ok()) {
        socket_lifecycle::close(result);
        errno = setup.native_error == 0 ? EIO : setup.native_error;
        return platform_types::invalid_socket;
    }
    return result;
}

} // namespace easy_uds::detail::platform_windows
#endif
