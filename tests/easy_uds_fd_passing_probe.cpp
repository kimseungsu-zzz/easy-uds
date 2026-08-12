#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/mman.h>
#endif

namespace {
#if defined(__linux__) && defined(MFD_CLOEXEC)
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
#endif

} // namespace

int main() {
#if defined(__linux__) && defined(MFD_CLOEXEC)
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        fail("socketpair");
    }
    const int payload_fd = ::memfd_create("easy-uds-fd-probe", MFD_CLOEXEC);
    if (payload_fd < 0) {
        (void)::close(sockets[0]);
        (void)::close(sockets[1]);
        fail("memfd_create");
    }
    const char payload[] = "fd-passing-ok";
    if (::write(payload_fd, payload, sizeof(payload)) != static_cast<ssize_t>(sizeof(payload))) {
        fail("write");
    }
    char marker = 'F';
    iovec vector{&marker, sizeof(marker)};
    char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &payload_fd, sizeof(payload_fd));
    if (::sendmsg(sockets[0], &message, 0) != 1) {
        fail("sendmsg");
    }

    char received_marker = 0;
    iovec received_vector{&received_marker, sizeof(received_marker)};
    char received_control[CMSG_SPACE(sizeof(int))]{};
    msghdr received{};
    received.msg_iov = &received_vector;
    received.msg_iovlen = 1;
    received.msg_control = received_control;
    received.msg_controllen = sizeof(received_control);
    if (::recvmsg(sockets[1], &received, 0) != 1) {
        fail("recvmsg");
    }
    cmsghdr* received_header = CMSG_FIRSTHDR(&received);
    if (received_marker != 'F' || received_header == nullptr ||
        received_header->cmsg_level != SOL_SOCKET || received_header->cmsg_type != SCM_RIGHTS) {
        throw std::runtime_error("invalid SCM_RIGHTS message");
    }
    int received_fd = -1;
    std::memcpy(&received_fd, CMSG_DATA(received_header), sizeof(received_fd));
    char readback[sizeof(payload)]{};
    if (::lseek(received_fd, 0, SEEK_SET) < 0 ||
        ::read(received_fd, readback, sizeof(readback)) != static_cast<ssize_t>(sizeof(readback)) ||
        std::memcmp(readback, payload, sizeof(payload)) != 0) {
        fail("read passed fd");
    }
    (void)::close(received_fd);
    (void)::close(payload_fd);
    (void)::close(sockets[0]);
    (void)::close(sockets[1]);
    std::cout << "SCM_RIGHTS + memfd_create: supported\n";
    return 0;
#else
    std::cout << "SCM_RIGHTS + memfd_create: unavailable on this platform\n";
    return 0;
#endif
}
