#include "../endpoint.hpp"
#include "../socket_lifecycle.hpp"

#include <cstddef>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/socket.h>
#include <unistd.h>

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
    endpoint.length = static_cast<decltype(endpoint.length)>(
        offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
    endpoint.address.sun_len = static_cast<unsigned char>(endpoint.length);
#endif
    return endpoint;
}

NativeSocket create_stream_socket() noexcept {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const auto close_on_exec = socket_lifecycle::set_close_on_exec(fd);
    const auto nonblocking = socket_lifecycle::set_nonblocking(fd);
    const auto no_sigpipe = socket_lifecycle::configure_no_sigpipe(fd);
    if (!close_on_exec.ok() || !nonblocking.ok() || !no_sigpipe.ok()) {
        const int error = !close_on_exec.ok() ? close_on_exec.native_error
                          : !nonblocking.ok() ? nonblocking.native_error
                                              : no_sigpipe.native_error;
        socket_lifecycle::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

int connect_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept {
    return ::connect(static_cast<int>(socket),
                     reinterpret_cast<const sockaddr*>(&endpoint.address),
                     static_cast<socklen_t>(endpoint.length));
}

int bind_socket(NativeSocket socket, const UnixEndpoint& endpoint) noexcept {
    return ::bind(static_cast<int>(socket),
                  reinterpret_cast<const sockaddr*>(&endpoint.address),
                  static_cast<socklen_t>(endpoint.length));
}

int listen_socket(NativeSocket socket, int backlog) noexcept {
    return ::listen(static_cast<int>(socket), backlog);
}

NativeSocket accept_socket(NativeSocket listener) noexcept {
    const int fd = ::accept(static_cast<int>(listener), nullptr, nullptr);
    if (fd < 0) {
        return -1;
    }
    const auto close_on_exec = socket_lifecycle::set_close_on_exec(fd);
    const auto nonblocking = socket_lifecycle::set_nonblocking(fd);
    const auto no_sigpipe = socket_lifecycle::configure_no_sigpipe(fd);
    if (!close_on_exec.ok() || !nonblocking.ok() || !no_sigpipe.ok()) {
        const int error = !close_on_exec.ok() ? close_on_exec.native_error
                          : !nonblocking.ok() ? nonblocking.native_error
                                              : no_sigpipe.native_error;
        socket_lifecycle::close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

} // namespace easy_uds::detail::platform_linux
