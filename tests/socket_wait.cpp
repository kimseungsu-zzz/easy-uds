#include "platform/socket_wait.hpp"

#include <cerrno>
#include <iostream>

#include <sys/socket.h>
#include <unistd.h>

int main() {
    using easy_uds::detail::socket_wait::Interest;
    using easy_uds::detail::socket_wait::Status;

    const auto require = [](bool condition, const char* message) {
        if (!condition) {
            std::cerr << message << '\n';
            return false;
        }
        return true;
    };

    int sockets[2] = {-1, -1};
    if (!require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                 "socketpair failed")) {
        return 1;
    }

    const auto timeout = easy_uds::detail::socket_wait::wait_once(
        sockets[0], Interest::read, 0);
    if (!require(timeout.status == Status::timed_out,
                 "empty socket did not time out")) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return 1;
    }

    const char byte = 'x';
    if (!require(::write(sockets[1], &byte, sizeof(byte)) == 1,
                 "socket write failed")) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return 1;
    }
    const auto ready = easy_uds::detail::socket_wait::wait_once(
        sockets[0], Interest::read, 1000);
    char received = 0;
    const ssize_t bytes_read = ::read(sockets[0], &received, sizeof(received));
    if (!require(ready.status == Status::ready,
                 "readable socket was not reported ready") ||
        !require(bytes_read == 1, "socket read failed") ||
        !require(received == byte, "socket byte round-trip failed")) {
        ::close(sockets[0]);
        ::close(sockets[1]);
        return 1;
    }

    ::close(sockets[0]);
    const auto invalid = easy_uds::detail::socket_wait::wait_once(
        sockets[0], Interest::write, 0);
    ::close(sockets[1]);
    return require(invalid.status == Status::invalid_descriptor &&
                       invalid.native_error == EBADF,
                   "invalid descriptor was not reported")
               ? 0
               : 1;
}
