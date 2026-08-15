#include "../descriptor_passing.hpp"

#include "easy_uds/error.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easy_uds::detail::descriptor_passing {
namespace {

#ifndef MSG_CMSG_CLOEXEC
[[noreturn]] void throw_cloexec_error(const char* operation, int error) {
    const std::error_code system_code(error, std::generic_category());
    throw easy_uds::Error(easy_uds::detail::classify_system_error(system_code),
                          operation, system_code);
}
#endif

#ifndef MSG_CMSG_CLOEXEC
void set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        throw_cloexec_error("fcntl(F_GETFD) failed", errno);
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw_cloexec_error("fcntl(F_SETFD) failed", errno);
    }
}
#endif

} // namespace

ssize_t send_iovecs(int fd, iovec* parts, std::size_t part_count, int passed_fd,
                    bool attach_fd) noexcept {
    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = part_count;
    char control[CMSG_SPACE(sizeof(int))]{};
    if (attach_fd) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* const cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &passed_fd, sizeof(passed_fd));
    }
#ifdef MSG_NOSIGNAL
    return ::sendmsg(fd, &message, MSG_NOSIGNAL);
#else
    return ::sendmsg(fd, &message, 0);
#endif
}

ssize_t receive(int fd, void* data, std::size_t size, int& received_fd) {
    received_fd = -1;
    iovec vector{data, size};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    int receive_flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    receive_flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t result = ::recvmsg(fd, &message, receive_flags);
    if (result <= 0) {
        return result;
    }

    int captured_fd = -1;
    bool malformed = false;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        if (header->cmsg_len != CMSG_LEN(sizeof(int)) || captured_fd >= 0) {
            malformed = true;
            continue;
        }
        std::memcpy(&captured_fd, CMSG_DATA(header), sizeof(captured_fd));
    }
    if ((message.msg_flags & MSG_CTRUNC) != 0 || malformed) {
        if (captured_fd >= 0) {
            (void)::close(captured_fd);
        }
        throw std::runtime_error("invalid or truncated ancillary descriptor");
    }
    if (captured_fd >= 0) {
#ifndef MSG_CMSG_CLOEXEC
        try {
            set_close_on_exec(captured_fd);
        } catch (...) {
            (void)::close(captured_fd);
            throw;
        }
#endif
        received_fd = captured_fd;
    }
    return result;
}

} // namespace easy_uds::detail::descriptor_passing
