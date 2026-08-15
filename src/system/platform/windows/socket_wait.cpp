#include "../socket_wait.hpp"
#include "socket_common.hpp"

#if defined(_WIN32)
#include <cerrno>

namespace easy_uds::detail::socket_wait {

Result wait_once(platform_types::NativeSocket fd, Interest interest,
                 int timeout_ms) noexcept {
    WSAPOLLFD item{};
    item.fd = platform_windows::to_socket(fd);
    item.events = interest == Interest::read ? POLLRDNORM : POLLWRNORM;
    const int result = ::WSAPoll(&item, 1, timeout_ms < 0 ? -1 : timeout_ms);
    if (result == SOCKET_ERROR) {
        const int error = platform_windows::last_wsa_error();
        if (error == WSAEINTR) {
            return {Status::interrupted, error};
        }
        return {Status::error, error};
    }
    if (result == 0) {
        return {Status::timed_out, 0};
    }
    if ((item.revents & POLLNVAL) != 0) {
        platform_windows::set_errno_from_wsa(WSAENOTSOCK);
        return {Status::invalid_descriptor, errno};
    }
    if ((item.revents & (POLLERR | POLLHUP | POLLRDNORM | POLLWRNORM)) != 0) {
        return {Status::ready, 0};
    }
    return {Status::ready, 0};
}

} // namespace easy_uds::detail::socket_wait
#endif
