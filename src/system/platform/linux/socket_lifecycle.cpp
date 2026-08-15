#include "../socket_lifecycle.hpp"

#include <cerrno>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easy_uds::detail::socket_lifecycle {

void close(NativeSocket fd) noexcept {
    if (fd >= 0) {
        (void)::close(fd);
    }
}

void shutdown(NativeSocket fd) noexcept {
    if (fd >= 0) {
        (void)::shutdown(fd, SHUT_RDWR);
    }
}

SetupResult set_close_on_exec(NativeSocket fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        return {errno, SetupFailure::close_on_exec_getfd};
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return {errno, SetupFailure::close_on_exec_setfd};
    }
    return {};
}

SetupResult set_nonblocking(NativeSocket fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return {errno, SetupFailure::nonblocking_getfl};
    }
    if ((flags & O_NONBLOCK) != 0) {
        return {};
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return {errno, SetupFailure::nonblocking_setfl};
    }
    return {};
}

SetupResult configure_no_sigpipe(NativeSocket fd) noexcept {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return {errno, SetupFailure::no_sigpipe};
    }
#else
    (void)fd;
#endif
    return {};
}

} // namespace easy_uds::detail::socket_lifecycle
