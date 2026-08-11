#pragma once

// Shared implementation infrastructure for the reactor server and the client:
// RAII descriptors, deadlines, non-blocking read-ahead I/O, and socket helpers.

#include "easy_uds/easy_uds.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace easy_uds::detail {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
namespace protocol = easy_uds::detail::protocol;
using protocol::HeaderBytes;
using protocol::WireType;

[[noreturn]] inline void throw_system_error(const char* operation, int error = errno) {
    throw std::system_error(error, std::generic_category(), operation);
}

inline void validate_nonnegative_timeout(std::chrono::milliseconds timeout, const char* name) {
    if (timeout.count() < 0) {
        throw std::invalid_argument(std::string(name) + " must not be negative");
    }
}

inline void validate_max_message_size(std::size_t size) {
    if (size == 0 || size > protocol::max_wire_field) {
        throw std::invalid_argument("max_message_size must be between 1 and UINT32_MAX bytes");
    }
}

inline void validate_stream_options(std::size_t chunk_size, std::chrono::milliseconds stream_timeout) {
    if (chunk_size == 0 || chunk_size > protocol::max_wire_field) {
        throw std::invalid_argument("stream_chunk_size must be between 1 and UINT32_MAX bytes");
    }
    validate_nonnegative_timeout(stream_timeout, "stream_timeout");
}

inline void validate_server_options(const easy_uds::ServerOptions& options) {
    validate_max_message_size(options.max_message_size);
    if (options.worker_threads == 0) {
        throw std::invalid_argument("worker_threads must be greater than zero");
    }
    if (options.max_connections == 0) {
        throw std::invalid_argument("max_connections must be greater than zero");
    }
    if (options.worker_threads > options.max_connections) {
        throw std::invalid_argument("worker_threads must not exceed max_connections");
    }
    if (options.max_concurrent_streams > options.worker_threads) {
        throw std::invalid_argument("max_concurrent_streams must not exceed worker_threads");
    }
    if (options.listen_backlog <= 0) {
        throw std::invalid_argument("listen_backlog must be greater than zero");
    }
    validate_nonnegative_timeout(options.io_timeout, "io_timeout");
    validate_nonnegative_timeout(options.request_timeout, "request_timeout");
    validate_stream_options(options.stream_chunk_size, options.stream_timeout);
    validate_nonnegative_timeout(options.session_idle_grace, "session_idle_grace");
    validate_nonnegative_timeout(options.stale_socket_grace_period, "stale_socket_grace_period");
    if (options.socket_permissions > 0777U) {
        throw std::invalid_argument("socket_permissions must be an octal permission mask from 0000 to 0777");
    }
}

inline void validate_client_options(const easy_uds::ClientOptions& options) {
    validate_max_message_size(options.max_message_size);
    validate_nonnegative_timeout(options.connect_timeout, "connect_timeout");
    validate_nonnegative_timeout(options.io_timeout, "io_timeout");
    validate_nonnegative_timeout(options.request_timeout, "request_timeout");
    validate_stream_options(options.stream_chunk_size, options.stream_timeout);
}

class FileDescriptor {
  public:
    explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_;
};

inline void set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        throw_system_error("fcntl(F_GETFD) failed");
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw_system_error("fcntl(F_SETFD) failed");
    }
}

inline void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        throw_system_error("fcntl(F_GETFL) failed");
    }
    if ((flags & O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw_system_error("fcntl(F_SETFL O_NONBLOCK) failed");
    }
}

inline void configure_no_sigpipe(int fd) {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        throw_system_error("setsockopt(SO_NOSIGPIPE) failed");
    }
#else
    (void)fd;
#endif
}

inline FileDescriptor make_socket() {
    int socket_type = SOCK_STREAM;
#if defined(__linux__) && defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    socket_type |= SOCK_CLOEXEC | SOCK_NONBLOCK;
#endif
    FileDescriptor fd(::socket(AF_UNIX, socket_type, 0));
    if (fd.get() < 0) {
        throw_system_error("socket failed");
    }
#if !defined(__linux__) || !defined(SOCK_CLOEXEC) || !defined(SOCK_NONBLOCK)
    set_close_on_exec(fd.get());
    set_nonblocking(fd.get());
#endif
    configure_no_sigpipe(fd.get());
    return fd;
}

inline sockaddr_un make_address(const std::string& socket_path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    if (socket_path.empty()) {
        throw std::invalid_argument("socket path must not be empty");
    }
    if (socket_path.find('\0') != std::string::npos) {
        throw std::invalid_argument("pathname socket path must not contain embedded NUL bytes");
    }
    if (socket_path.size() >= sizeof(address.sun_path)) {
        throw std::invalid_argument("socket path is too long");
    }

    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    return address;
}

inline socklen_t address_length(const sockaddr_un& address) {
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1);
}

inline Deadline deadline_from_now(std::chrono::milliseconds timeout) {
    if (timeout.count() == 0) {
        return Deadline::max();
    }
    const auto now = Clock::now();
    const auto max_remaining = Deadline::max() - now;
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_remaining);
    if (timeout >= max_ms) {
        return Deadline::max() - Clock::duration{1};
    }
    return now + timeout;
}

inline Deadline earlier_deadline(Deadline left, Deadline right) noexcept {
    return left < right ? left : right;
}

inline int poll_timeout_ms(Deadline deadline) {
    if (deadline == Deadline::max()) {
        return -1;
    }
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto bounded = std::min<std::int64_t>(remaining.count(), std::numeric_limits<int>::max());
    return std::max(1, static_cast<int>(bounded));
}

inline void check_absolute_deadline(Deadline deadline, const char* timeout_operation) {
    if (deadline != Deadline::max() && Clock::now() >= deadline) {
        throw_system_error(timeout_operation, ETIMEDOUT);
    }
}

inline void wait_for_io(int fd, short events, std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline,
                        const char* timeout_operation) {
    Deadline wait_deadline = absolute_deadline;
    if (inactivity_timeout.count() > 0) {
        wait_deadline = earlier_deadline(wait_deadline, deadline_from_now(inactivity_timeout));
    }

    while (true) {
        const int timeout_ms = poll_timeout_ms(wait_deadline);
        if (timeout_ms == 0) {
            throw_system_error(timeout_operation, ETIMEDOUT);
        }

        pollfd item{};
        item.fd = fd;
        item.events = events;
        const int result = ::poll(&item, 1, timeout_ms);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_system_error("poll failed");
        }
        if (result == 0) {
            throw_system_error(timeout_operation, ETIMEDOUT);
        }
        if ((item.revents & POLLNVAL) != 0) {
            throw_system_error("socket descriptor became invalid", EBADF);
        }
        if ((item.revents & (events | POLLERR | POLLHUP)) != 0) {
            return;
        }
    }
}

// Read-ahead buffering for the read path. Small logical reads are served from
// a shared per-connection buffer so one recv() can satisfy header+payload
// sequences; demands of at least the buffer size fall through to a direct
// recv() so large bodies keep a single-pass syscall cost.
inline constexpr std::size_t read_ahead_capacity = 4096;

class BufferedReader {
  public:
    explicit BufferedReader(int fd) noexcept : fd_(fd) {}

    // Reads exactly `size` bytes into `data`.
    void read(void* data, std::size_t size, std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
        auto* bytes = static_cast<char*>(data);
        std::size_t received = 0;
        while (received < size) {
            if (size_ != 0) {
                const std::size_t take = std::min(size - received, size_);
                std::memcpy(bytes + received, buffer_.data() + start_, take);
                start_ += take;
                size_ -= take;
                received += take;
                continue;
            }
            if (size - received >= buffer_.size()) {
                // read_direct delivers every remaining byte, so the read is done.
                read_direct(bytes + received, size - received, inactivity_timeout, absolute_deadline);
                return;
            }
            start_ = 0;
            size_ = static_cast<std::size_t>(
                receive(buffer_.data(), buffer_.size(), inactivity_timeout, absolute_deadline));
        }
    }

    [[nodiscard]] bool buffered() const noexcept { return size_ != 0; }

  private:
    ssize_t receive(void* data, std::size_t capacity, std::chrono::milliseconds inactivity_timeout,
                    Deadline absolute_deadline) {
        while (true) {
            check_absolute_deadline(absolute_deadline, "receive timed out");
            const ssize_t result = ::recv(fd_, data, capacity, 0);
            if (result > 0) {
                return result;
            }
            if (result == 0) {
                throw_system_error("peer closed connection", ECONNRESET);
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_for_io(fd_, POLLIN, inactivity_timeout, absolute_deadline, "receive timed out");
                continue;
            }
            throw_system_error("receive failed");
        }
    }

    void read_direct(char* data, std::size_t size, std::chrono::milliseconds inactivity_timeout,
                     Deadline absolute_deadline) {
        while (size != 0) {
            const std::size_t received =
                static_cast<std::size_t>(receive(data, size, inactivity_timeout, absolute_deadline));
            data += received;
            size -= received;
        }
    }

    int fd_;
    std::array<char, read_ahead_capacity> buffer_{};
    std::size_t start_ = 0;
    std::size_t size_ = 0;
};

inline void write_exact(int fd, const void* data, std::size_t size, std::chrono::milliseconds inactivity_timeout,
                        Deadline absolute_deadline) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;

    while (sent < size) {
        check_absolute_deadline(absolute_deadline, "send timed out");
#ifdef MSG_NOSIGNAL
        const ssize_t result = ::send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
#else
        const ssize_t result = ::send(fd, bytes + sent, size - sent, 0);
#endif
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            throw_system_error("send failed", EPIPE);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_for_io(fd, POLLOUT, inactivity_timeout, absolute_deadline, "send timed out");
            continue;
        }
        throw_system_error("send failed");
    }
}

inline void write_iovecs_exact(int fd, iovec* parts, std::size_t part_count,
                               std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    std::size_t first = 0;
    while (first < part_count) {
        while (first < part_count && parts[first].iov_len == 0) {
            ++first;
        }
        if (first == part_count) {
            return;
        }

        check_absolute_deadline(absolute_deadline, "send timed out");
        msghdr message{};
        message.msg_iov = parts + first;
        message.msg_iovlen = part_count - first;
#ifdef MSG_NOSIGNAL
        const ssize_t result = ::sendmsg(fd, &message, MSG_NOSIGNAL);
#else
        const ssize_t result = ::sendmsg(fd, &message, 0);
#endif
        if (result > 0) {
            std::size_t consumed = static_cast<std::size_t>(result);
            while (first < part_count && consumed >= parts[first].iov_len) {
                consumed -= parts[first].iov_len;
                ++first;
            }
            if (consumed != 0 && first < part_count) {
                auto* base = static_cast<unsigned char*>(parts[first].iov_base);
                parts[first].iov_base = base + consumed;
                parts[first].iov_len -= consumed;
            }
            continue;
        }
        if (result == 0) {
            throw_system_error("send failed", EPIPE);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_for_io(fd, POLLOUT, inactivity_timeout, absolute_deadline, "send timed out");
            continue;
        }
        throw_system_error("send failed");
    }
}

inline void write_frame_with_payload(int fd, WireType type, std::uint32_t request_id, std::uint32_t arg1,
                                     std::uint32_t arg2, const void* payload, std::size_t payload_size,
                                     std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    HeaderBytes header = protocol::encode_header(type, request_id, arg1, arg2);
    std::array<iovec, 2> parts{{
        {header.data(), header.size()},
        {const_cast<void*>(payload), payload_size},
    }};
    write_iovecs_exact(fd, parts.data(), parts.size(), inactivity_timeout, absolute_deadline);
}

inline void write_header_frame(int fd, WireType type, std::uint32_t request_id, std::uint32_t arg1,
                               std::uint32_t arg2, std::chrono::milliseconds inactivity_timeout,
                               Deadline absolute_deadline) {
    const HeaderBytes header = protocol::encode_header(type, request_id, arg1, arg2);
    write_exact(fd, header.data(), header.size(), inactivity_timeout, absolute_deadline);
}

inline void connect_nonblocking(int fd, const sockaddr_un& address, std::chrono::milliseconds connect_timeout,
                                Deadline request_deadline) {
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length(address)) == 0) {
        return;
    }

    const int connect_error = errno;
    if (connect_error != EINPROGRESS && connect_error != EAGAIN && connect_error != EWOULDBLOCK &&
        connect_error != EINTR) {
        throw_system_error("connect failed", connect_error);
    }

    Deadline deadline = request_deadline;
    if (connect_timeout.count() > 0) {
        deadline = earlier_deadline(deadline, deadline_from_now(connect_timeout));
    }

    wait_for_io(fd, POLLOUT, std::chrono::milliseconds{0}, deadline, "connect timed out");

    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
        throw_system_error("getsockopt(SO_ERROR) failed");
    }
    if (socket_error != 0) {
        throw_system_error("connect failed", socket_error);
    }
}

// Captures SO_PEERCRED (Linux) / getpeereid (BSD) for a connected socket.
inline easy_uds::PeerCredentials capture_peer_credentials(int fd) noexcept {
    easy_uds::PeerCredentials peer;
#if defined(SO_PEERCRED)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0) {
        peer.pid = static_cast<pid_t>(credentials.pid);
        peer.uid = static_cast<uid_t>(credentials.uid);
        peer.gid = static_cast<gid_t>(credentials.gid);
        peer.present = true;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) == 0) {
        peer.pid = -1;
        peer.uid = static_cast<uid_t>(uid);
        peer.gid = static_cast<gid_t>(gid);
        peer.present = true;
    }
#else
    (void)fd;
#endif
    return peer;
}

} // namespace easy_uds::detail