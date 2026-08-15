#include "../socket_io.hpp"

#include <sys/socket.h>

namespace easy_uds::detail::socket_io {

ssize_t receive(int fd, void* data, std::size_t size) noexcept {
    return ::recv(fd, data, size, 0);
}

ssize_t send(int fd, const void* data, std::size_t size) noexcept {
#ifdef MSG_NOSIGNAL
    return ::send(fd, data, size, MSG_NOSIGNAL);
#else
    return ::send(fd, data, size, 0);
#endif
}

ssize_t send_iovecs(int fd, iovec* parts, std::size_t part_count) noexcept {
    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = part_count;
#ifdef MSG_NOSIGNAL
    return ::sendmsg(fd, &message, MSG_NOSIGNAL);
#else
    return ::sendmsg(fd, &message, 0);
#endif
}

ssize_t send_iovecs_nonblocking(int fd, iovec* parts,
                                std::size_t part_count) noexcept {
    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = part_count;
#ifdef MSG_NOSIGNAL
    return ::sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
#else
    return ::sendmsg(fd, &message, MSG_DONTWAIT);
#endif
}

int query_socket_error(int fd, int& socket_error) noexcept {
    socklen_t length = sizeof(socket_error);
    return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length);
}

} // namespace easy_uds::detail::socket_io
