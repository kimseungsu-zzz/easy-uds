#include "platform/descriptor_passing.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void close_checked(int fd) {
    if (fd >= 0) {
        (void)::close(fd);
    }
}

std::size_t open_fd_count() {
    std::size_t count = 0;
    if (DIR* directory = ::opendir("/proc/self/fd")) {
        while (::readdir(directory) != nullptr) {
            ++count;
        }
        (void)::closedir(directory);
    }
    return count;
}

void send_multiple_rights(int socket, const int* descriptors, std::size_t count,
                          bool separate_messages) {
    require(count == 2, "test helper expects two descriptors");
    const std::size_t control_size =
        separate_messages ? CMSG_SPACE(sizeof(int)) * 2 : CMSG_SPACE(sizeof(int) * 2);
    std::string control(control_size, '\0');
    char byte = 'x';
    iovec vector{&byte, sizeof(byte)};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();

    cmsghdr* first = CMSG_FIRSTHDR(&message);
    require(first != nullptr, "failed to create first ancillary header");
    first->cmsg_level = SOL_SOCKET;
    first->cmsg_type = SCM_RIGHTS;
    first->cmsg_len = CMSG_LEN(separate_messages ? sizeof(int) : sizeof(int) * 2);
    std::memcpy(CMSG_DATA(first), descriptors, separate_messages ? sizeof(int) : sizeof(int) * 2);
    if (separate_messages) {
        cmsghdr* second = CMSG_NXTHDR(&message, first);
        require(second != nullptr, "failed to create duplicate ancillary header");
        second->cmsg_level = SOL_SOCKET;
        second->cmsg_type = SCM_RIGHTS;
        second->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(second), descriptors + 1, sizeof(int));
    }

    ssize_t result = -1;
    do {
        result = ::sendmsg(socket, &message, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "malformed descriptor send failed");
    }
    require(result == 1, "malformed descriptor helper sent an unexpected byte count");
}

void expect_rejected(bool separate_messages, const char* message) {
    const std::size_t before = open_fd_count();
    int descriptors[2] = {-1, -1};
    require(::pipe(descriptors) == 0, "pipe failed for malformed descriptor");
    int sockets[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair failed for malformed descriptor");
    try {
        send_multiple_rights(sockets[0], descriptors, 2, separate_messages);
        char byte = 0;
        int received = -1;
        bool rejected = false;
        try {
            const auto result = easy_uds::detail::descriptor_passing::receive(
                sockets[1], &byte, sizeof(byte));
            received = result.received_fd;
            rejected = result.error ==
                       easy_uds::detail::descriptor_passing::ReceiveError::invalid_ancillary;
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, message);
        require(received == -1, "rejected ancillary data returned a descriptor");
    } catch (...) {
        close_checked(sockets[0]);
        close_checked(sockets[1]);
        close_checked(descriptors[0]);
        close_checked(descriptors[1]);
        throw;
    }
    close_checked(sockets[0]);
    close_checked(sockets[1]);
    close_checked(descriptors[0]);
    close_checked(descriptors[1]);
    const std::size_t after = open_fd_count();
    require(after == before,
            "rejected ancillary data leaked a materialized descriptor");
}

void test_round_trip_and_ownership() {
    int sockets[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair failed for descriptor round-trip");
    int pipe_fds[2] = {-1, -1};
    require(::pipe(pipe_fds) == 0, "pipe failed for descriptor round-trip");
    const char payload = 'x';
    require(::write(pipe_fds[1], &payload, sizeof(payload)) == 1,
            "pipe write failed for descriptor round-trip");

    iovec vector{const_cast<char*>(&payload), sizeof(payload)};
    const ssize_t sent = easy_uds::detail::descriptor_passing::send_iovecs(
        sockets[0], &vector, 1, pipe_fds[0], true);
    require(sent == 1, "descriptor send did not make progress");
    require(::fcntl(pipe_fds[0], F_GETFD) >= 0,
            "sending a descriptor consumed caller ownership");

    char received_byte = 0;
    const auto receive_result = easy_uds::detail::descriptor_passing::receive(
        sockets[1], &received_byte, sizeof(received_byte));
    const int received_fd = receive_result.received_fd;
    require(receive_result.bytes == 1 && receive_result.error ==
                easy_uds::detail::descriptor_passing::ReceiveError::none,
            "descriptor receive did not make progress");
    require(received_byte == payload && received_fd >= 0,
            "descriptor round-trip returned the wrong payload");
    require((::fcntl(received_fd, F_GETFD) & FD_CLOEXEC) != 0,
            "received descriptor is not close-on-exec");
    close_checked(pipe_fds[0]);
    char from_received = 0;
    require(::read(received_fd, &from_received, sizeof(from_received)) == 1 &&
                from_received == payload,
            "receiver did not own a usable descriptor");
    close_checked(received_fd);
    close_checked(pipe_fds[1]);
    close_checked(sockets[0]);
    close_checked(sockets[1]);
}

void test_repeated_round_trips_do_not_leak() {
    int sockets[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "socketpair failed for descriptor leak test");
    const std::size_t before = open_fd_count();
    for (int iteration = 0; iteration < 64; ++iteration) {
        int pipe_fds[2] = {-1, -1};
        require(::pipe(pipe_fds) == 0, "pipe failed for descriptor leak test");
        const char byte = 'x';
        iovec vector{const_cast<char*>(&byte), sizeof(byte)};
        require(easy_uds::detail::descriptor_passing::send_iovecs(
                    sockets[0], &vector, 1, pipe_fds[0], true) == 1,
                "descriptor leak test send failed");
        close_checked(pipe_fds[0]);
        close_checked(pipe_fds[1]);
        char received_byte = 0;
        const auto receive_result = easy_uds::detail::descriptor_passing::receive(
            sockets[1], &received_byte, sizeof(received_byte));
        const int received_fd = receive_result.received_fd;
        require(receive_result.bytes == 1 && receive_result.error ==
                    easy_uds::detail::descriptor_passing::ReceiveError::none,
                "descriptor leak test receive failed");
        require(received_fd >= 0, "descriptor leak test lost descriptor");
        close_checked(received_fd);
    }
    const std::size_t after = open_fd_count();
    close_checked(sockets[0]);
    close_checked(sockets[1]);
    require(after == before, "repeated descriptor round-trips leaked descriptors");
}

} // namespace

int main() {
    try {
        test_round_trip_and_ownership();
        expect_rejected(true, "duplicate ancillary descriptors were accepted");
        expect_rejected(false, "truncated ancillary descriptors were accepted");
        test_repeated_round_trips_do_not_leak();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "descriptor_passing_test: %s\n", error.what());
        return 1;
    }
}
