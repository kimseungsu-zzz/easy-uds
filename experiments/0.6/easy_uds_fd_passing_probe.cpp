#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

// Creates a memfd pre-filled with `payload`, positioned at offset 0.
int make_payload_fd(const std::string& payload) {
    const int fd = ::memfd_create("easy-uds-fd-probe", MFD_CLOEXEC);
    if (fd < 0) {
        fail("memfd_create");
    }
    if (::write(fd, payload.data(), payload.size()) != static_cast<ssize_t>(payload.size())) {
        fail("write");
    }
    return fd;
}

// Sends one byte on `sock`, attaching `fd` as SCM_RIGHTS when `fd >= 0`.
void send_frame(int sock, char byte, int fd) {
    iovec vector{&byte, sizeof(byte)};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    if (fd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    }
    if (::sendmsg(sock, &message, 0) != 1) {
        fail("sendmsg");
    }
}

// Reads exactly `count` bytes into `out` using recvmsg with a control buffer,
// returning the first received fd (or -1 when none arrived).
int recv_bytes_with_fd(int sock, std::vector<char>& out, std::size_t count) {
    out.resize(count);
    iovec vector{out.data(), count};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    std::size_t obtained = 0;
    int received_fd = -1;
    while (obtained < count) {
        const ssize_t result = ::recvmsg(sock, &message, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("recvmsg");
        }
        obtained += static_cast<std::size_t>(result);
        if (received_fd < 0) {
            cmsghdr* header = CMSG_FIRSTHDR(&message);
            if (header != nullptr && header->cmsg_level == SOL_SOCKET &&
                header->cmsg_type == SCM_RIGHTS && header->cmsg_len >= CMSG_LEN(sizeof(int))) {
                std::memcpy(&received_fd, CMSG_DATA(header), sizeof(received_fd));
            }
        }
        if (obtained < count) {
            // Move the iov forward for the next recvmsg; easiest: re-point.
            vector.iov_base = out.data() + obtained;
            vector.iov_len = count - obtained;
        }
    }
    return received_fd;
}
#endif
} // namespace

int main() {
#if defined(__linux__) && defined(MFD_CLOEXEC)
    const std::string payload_b = "read-ahead-fd-B";
    const std::string payload_d = "dropped-by-plain-read";

    // Scenario 1: one byte + fd in a single exchange (original probe).
    {
        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            fail("socketpair");
        }
        const int payload_fd = make_payload_fd("fd-passing-ok");
        send_frame(sockets[0], 'F', payload_fd);

        std::vector<char> bytes;
        const int received_fd = recv_bytes_with_fd(sockets[1], bytes, 1);
        if (bytes.size() != 1 || bytes[0] != 'F' || received_fd < 0) {
            throw std::runtime_error("scenario 1: fd not delivered with byte");
        }
        constexpr std::size_t payload_size = sizeof("fd-passing-ok") - 1;
        char readback[16]{};
        if (::lseek(received_fd, 0, SEEK_SET) < 0 ||
            ::read(received_fd, readback, payload_size) != static_cast<ssize_t>(payload_size) ||
            std::memcmp(readback, "fd-passing-ok", payload_size) != 0) {
            fail("read passed fd");
        }
        (void)::close(received_fd);
        (void)::close(payload_fd);
        (void)::close(sockets[0]);
        (void)::close(sockets[1]);
    }

    // Scenario 2: the reactor read-ahead premise. Three back-to-back frames
    // ('A' no-fd, 'B' WITH fd, 'C' no-fd) are pulled by ONE recvmsg from a
    // freshly-connected pair, so the fd frame sits in the middle of a batch.
    // The fd must survive and be retrievable alongside the frame bytes.
    {
        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            fail("socketpair");
        }
        const int payload_fd = make_payload_fd(payload_b);
        send_frame(sockets[0], 'A', -1);
        send_frame(sockets[0], 'B', payload_fd);
        send_frame(sockets[0], 'C', -1);

        std::vector<char> bytes;
        const int received_fd = recv_bytes_with_fd(sockets[1], bytes, 3);
        if (bytes.size() != 3 || bytes[0] != 'A' || bytes[1] != 'B' || bytes[2] != 'C') {
            throw std::runtime_error("scenario 2: frame bytes scrambled");
        }
        if (received_fd < 0) {
            throw std::runtime_error("scenario 2: fd lost during read-ahead batch");
        }
        char readback[32]{};
        if (::lseek(received_fd, 0, SEEK_SET) < 0 ||
            ::read(received_fd, readback, sizeof(readback)) !=
                static_cast<ssize_t>(payload_b.size()) ||
            std::memcmp(readback, payload_b.data(), payload_b.size()) != 0) {
            fail("read read-ahead passed fd");
        }
        (void)::close(received_fd);
        (void)::close(payload_fd);
        (void)::close(sockets[0]);
        (void)::close(sockets[1]);
    }

    // Scenario 3: negative control. A plain recv() with no control buffer that
    // consumes the fd-carrying byte silently discards the fd; a subsequent
    // recvmsg cannot recover it. This is why the fd path must use recvmsg with
    // a control buffer and tell the reactor to expect ancillary data.
    {
        int sockets[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
            fail("socketpair");
        }
        const int payload_fd = make_payload_fd(payload_d);
        send_frame(sockets[0], 'D', payload_fd);

        char plain = 0;
        if (::read(sockets[1], &plain, 1) != 1 || plain != 'D') {
            fail("plain read");
        }

        // The fd-carrying byte was consumed by a plain read with no control
        // buffer: the fd was discarded at that boundary. Nothing (bytes or fd)
        // remains pending for a subsequent recvmsg.
        char probe = 0;
        iovec probe_vector{&probe, sizeof(probe)};
        msghdr probe_message{};
        probe_message.msg_iov = &probe_vector;
        probe_message.msg_iovlen = 1;
        char probe_control[CMSG_SPACE(sizeof(int))]{};
        probe_message.msg_control = probe_control;
        probe_message.msg_controllen = sizeof(probe_control);
        const ssize_t probe_result = ::recvmsg(sockets[1], &probe_message, MSG_DONTWAIT);
        if (probe_result != -1 || errno != EAGAIN) {
            throw std::runtime_error("scenario 3: fd or bytes unexpectedly still pending");
        }
        (void)::close(payload_fd);
        (void)::close(sockets[0]);
        (void)::close(sockets[1]);
    }

    std::cout << "SCM_RIGHTS: delivered, read-ahead-surviving, dropped-by-plain-read verified\n";
    return 0;
#else
    std::cout << "SCM_RIGHTS + memfd_create: unavailable on this platform\n";
    return 0;
#endif
}
