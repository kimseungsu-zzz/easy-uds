#include "../socket_wait.hpp"

#include <cerrno>

#include <poll.h>

namespace easy_uds::detail::socket_wait {

Result wait_once(int fd, Interest interest, int timeout_ms) noexcept {
    pollfd item{};
    item.fd = fd;
    item.events = interest == Interest::read ? POLLIN : POLLOUT;

    const int result = ::poll(&item, 1, timeout_ms);
    if (result < 0) {
        if (errno == EINTR) {
            return {Status::interrupted, EINTR};
        }
        return {Status::error, errno};
    }
    if (result == 0) {
        return {Status::timed_out, 0};
    }
    if ((item.revents & POLLNVAL) != 0) {
        return {Status::invalid_descriptor, EBADF};
    }
    return {Status::ready, 0};
}

} // namespace easy_uds::detail::socket_wait
