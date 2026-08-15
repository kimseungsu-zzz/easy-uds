#include "../descriptor_passing.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easy_uds::detail::descriptor_passing {
namespace {

// Linux limits one SCM_RIGHTS message to SCM_MAX_FD (253) descriptors. Keep
// enough control storage to materialize every accepted descriptor so a
// malformed/truncated message can close all kernel-created copies.
constexpr std::size_t max_received_descriptors = 253;

#ifndef MSG_CMSG_CLOEXEC
int set_close_on_exec(int fd, ReceiveError& error) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        error = ReceiveError::close_on_exec_getfd;
        return errno;
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        error = ReceiveError::close_on_exec_setfd;
        return errno;
    }
    return 0;
}
#endif

} // namespace

ssize_t send_iovecs(platform_types::NativeSocket fd, iovec* parts, std::size_t part_count,
                    platform_types::NativeSocket passed_fd,
                    bool attach_fd) noexcept {
    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = part_count;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
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

ReceiveResult receive(platform_types::NativeSocket fd, void* data, std::size_t size) {
    ReceiveResult output;
    iovec vector{data, size};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    // Reuse the large defensive control area per reactor thread. Descriptor
    // receive is also used for ordinary frames, so this must not add a heap
    // allocation or repeatedly zero a kilobyte-sized stack buffer.
    alignas(cmsghdr) thread_local
        std::array<char, CMSG_SPACE(sizeof(int) * max_received_descriptors)> control{};
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    int receive_flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    receive_flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t result = ::recvmsg(fd, &message, receive_flags);
    if (result <= 0) {
        output.bytes = result;
        return output;
    }

    // The count tracks every descriptor copied from a valid ancillary payload,
    // so the storage does not need to be initialized on the ordinary no-FD
    // path.  This keeps descriptor receive from adding a kilobyte memset to
    // every fixed request while still retaining all descriptors for cleanup
    // when ancillary validation fails.
    std::array<int, max_received_descriptors> captured_fds;
    std::size_t captured_count = 0;
    bool malformed = false;
    const auto* control_begin = reinterpret_cast<const unsigned char*>(control.data());
    const auto* control_end = control_begin + message.msg_controllen;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;) {
        const auto* header_bytes = reinterpret_cast<const unsigned char*>(header);
        if (header_bytes + sizeof(cmsghdr) > control_end ||
            header->cmsg_len < CMSG_LEN(0) ||
            header_bytes + header->cmsg_len > control_end) {
            malformed = true;
            break;
        }
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
            header = CMSG_NXTHDR(&message, header);
            continue;
        }
        const std::size_t payload_bytes = header->cmsg_len - CMSG_LEN(0);
        const std::size_t descriptor_count = payload_bytes / sizeof(int);
        if (payload_bytes == 0 || payload_bytes % sizeof(int) != 0 ||
            descriptor_count > max_received_descriptors - captured_count) {
            malformed = true;
            break;
        }
        std::memcpy(captured_fds.data() + captured_count, CMSG_DATA(header),
                    descriptor_count * sizeof(int));
        captured_count += descriptor_count;
        if (descriptor_count != 1 || captured_count != 1) {
            malformed = true;
        }
        header = CMSG_NXTHDR(&message, header);
    }
    if ((message.msg_flags & MSG_CTRUNC) != 0 || malformed) {
        for (std::size_t index = 0; index < captured_count; ++index) {
            (void)::close(captured_fds[index]);
        }
        output.bytes = result;
        output.error = ReceiveError::invalid_ancillary;
        return output;
    }
    if (captured_count == 1) {
#ifndef MSG_CMSG_CLOEXEC
        ReceiveError close_on_exec_error_kind = ReceiveError::none;
        const int close_on_exec_error =
            set_close_on_exec(captured_fds[0], close_on_exec_error_kind);
        if (close_on_exec_error != 0) {
            (void)::close(captured_fds[0]);
            output.bytes = result;
            output.error = close_on_exec_error_kind;
            output.native_error = close_on_exec_error;
            return output;
        }
#endif
        output.received_fd = captured_fds[0];
    }
    output.bytes = result;
    return output;
}

} // namespace easy_uds::detail::descriptor_passing
