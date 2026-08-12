#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
[[noreturn]] void fail(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

int make_payload(std::size_t bytes) {
    const int fd = ::memfd_create("easy-uds-zero-copy", MFD_CLOEXEC);
    if (fd < 0) fail("memfd_create");
    std::vector<char> chunk(64U * 1024U, 'z');
    for (std::size_t written = 0; written < bytes;) {
        const std::size_t count = std::min(chunk.size(), bytes - written);
        if (::write(fd, chunk.data(), count) != static_cast<ssize_t>(count)) fail("write payload");
        written += count;
    }
    return fd;
}

template <typename Sender>
double measure_send(Sender&& sender, int socket_fd, std::size_t bytes) {
    std::thread receiver([socket_fd, bytes] {
        std::vector<char> buffer(64U * 1024U);
        for (std::size_t received = 0; received < bytes;) {
            const ssize_t count = ::read(socket_fd, buffer.data(), buffer.size());
            if (count <= 0) fail("receive payload");
            received += static_cast<std::size_t>(count);
        }
    });
    const auto started = std::chrono::steady_clock::now();
    sender();
    receiver.join();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}
} // namespace

int main(int argc, char** argv) {
    const std::size_t bytes = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 16U * 1024U * 1024U;
    if (bytes == 0 || argc > 2) {
        std::cerr << "usage: easy_uds_zero_copy_probe [bytes]\n";
        return 2;
    }
    int sockets[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) fail("socketpair");
    const int source_fd = make_payload(bytes);
    const int copy_fd = make_payload(bytes);
    const int splice_fd = make_payload(bytes);
    if (::lseek(copy_fd, 0, SEEK_SET) < 0 || ::lseek(splice_fd, 0, SEEK_SET) < 0) {
        fail("prepare payload");
    }
    const double sendfile_seconds = measure_send([&] {
        off_t offset = 0;
        for (std::size_t sent = 0; sent < bytes;) {
            const ssize_t count = ::sendfile(sockets[0], source_fd, &offset, bytes - sent);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) fail("sendfile");
            sent += static_cast<std::size_t>(count);
        }
    }, sockets[1], bytes);
    const double splice_seconds = measure_send([&] {
        int pipe_fds[2] = {-1, -1};
        if (::pipe2(pipe_fds, O_CLOEXEC) != 0) fail("pipe2");
        for (std::size_t sent = 0; sent < bytes;) {
            const ssize_t moved = ::splice(splice_fd, nullptr, pipe_fds[1], nullptr,
                                           std::min<std::size_t>(64U * 1024U, bytes - sent),
                                           SPLICE_F_MOVE | SPLICE_F_MORE);
            if (moved < 0 && errno == EINTR) continue;
            if (moved <= 0) fail("splice file-to-pipe");
            std::size_t drained = 0;
            while (drained < static_cast<std::size_t>(moved)) {
                const ssize_t count = ::splice(pipe_fds[0], nullptr, sockets[0], nullptr,
                                               static_cast<std::size_t>(moved) - drained,
                                               SPLICE_F_MOVE | SPLICE_F_MORE);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) fail("splice pipe-to-socket");
                drained += static_cast<std::size_t>(count);
            }
            sent += static_cast<std::size_t>(moved);
        }
        (void)::close(pipe_fds[0]);
        (void)::close(pipe_fds[1]);
    }, sockets[1], bytes);
    const double copied_seconds = measure_send([&] {
        std::vector<char> buffer(64U * 1024U);
        for (std::size_t sent = 0; sent < bytes;) {
            const ssize_t loaded = ::read(copy_fd, buffer.data(), std::min(buffer.size(), bytes - sent));
            if (loaded <= 0) fail("read payload");
            for (ssize_t offset = 0; offset < loaded;) {
                const ssize_t count = ::write(sockets[0], buffer.data() + offset,
                                              static_cast<std::size_t>(loaded - offset));
                if (count <= 0) fail("write copied payload");
                offset += count;
            }
            sent += static_cast<std::size_t>(loaded);
        }
    }, sockets[1], bytes);
    (void)::close(source_fd); (void)::close(copy_fd); (void)::close(splice_fd);
    (void)::close(sockets[0]); (void)::close(sockets[1]);
    std::cout << "bytes=" << bytes << ", sendfile=" << sendfile_seconds
              << " s, splice=" << splice_seconds << " s, read_write=" << copied_seconds
              << " s, sendfile_speedup=" << copied_seconds / sendfile_seconds
              << "x, splice_speedup=" << copied_seconds / splice_seconds << "x\n";
    return 0;
}
