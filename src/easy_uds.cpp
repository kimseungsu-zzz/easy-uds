#include "easy_uds/easy_uds.hpp"

#include "protocol_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>

namespace easy_uds {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
namespace protocol = detail::protocol;
using protocol::HeaderBytes;
using protocol::WireType;

inline constexpr std::size_t max_accept_batch = 64;

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

[[noreturn]] void throw_system_error(const char* operation, int error = errno) {
    throw std::system_error(error, std::generic_category(), operation);
}

sockaddr_un make_address(const std::string& socket_path) {
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

socklen_t address_length(const sockaddr_un& address) {
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + std::strlen(address.sun_path) + 1);
}

void validate_nonnegative_timeout(std::chrono::milliseconds timeout, const char* name) {
    if (timeout.count() < 0) {
        throw std::invalid_argument(std::string(name) + " must not be negative");
    }
}

void validate_max_message_size(std::size_t size) {
    if (size == 0 || size > protocol::max_wire_field) {
        throw std::invalid_argument("max_message_size must be between 1 and UINT32_MAX bytes");
    }
}

void validate_stream_options(std::size_t chunk_size, std::chrono::milliseconds stream_timeout) {
    if (chunk_size == 0 || chunk_size > protocol::max_wire_field) {
        throw std::invalid_argument("stream_chunk_size must be between 1 and UINT32_MAX bytes");
    }
    validate_nonnegative_timeout(stream_timeout, "stream_timeout");
}

void validate_server_options(const ServerOptions& options) {
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
    if (options.listen_backlog <= 0) {
        throw std::invalid_argument("listen_backlog must be greater than zero");
    }
    validate_nonnegative_timeout(options.io_timeout, "io_timeout");
    validate_nonnegative_timeout(options.request_timeout, "request_timeout");
    validate_stream_options(options.stream_chunk_size, options.stream_timeout);
    validate_nonnegative_timeout(options.stale_socket_grace_period, "stale_socket_grace_period");
    if (options.socket_permissions > 0777U) {
        throw std::invalid_argument("socket_permissions must be an octal permission mask from 0000 to 0777");
    }
}

void validate_client_options(const ClientOptions& options) {
    validate_max_message_size(options.max_message_size);
    validate_nonnegative_timeout(options.connect_timeout, "connect_timeout");
    validate_nonnegative_timeout(options.io_timeout, "io_timeout");
    validate_nonnegative_timeout(options.request_timeout, "request_timeout");
    validate_stream_options(options.stream_chunk_size, options.stream_timeout);
}

void set_close_on_exec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        throw_system_error("fcntl(F_GETFD) failed");
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        throw_system_error("fcntl(F_SETFD) failed");
    }
}

void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        throw_system_error("fcntl(F_GETFL) failed");
    }
    if ((flags & O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw_system_error("fcntl(F_SETFL O_NONBLOCK) failed");
    }
}

void configure_no_sigpipe(int fd) {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        throw_system_error("setsockopt(SO_NOSIGPIPE) failed");
    }
#else
    (void)fd;
#endif
}

FileDescriptor make_socket() {
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

std::array<FileDescriptor, 2> make_wakeup_pipe() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        throw_system_error("pipe failed");
    }

    std::array<FileDescriptor, 2> result{FileDescriptor(fds[0]), FileDescriptor(fds[1])};
    set_close_on_exec(result[0].get());
    set_close_on_exec(result[1].get());
    set_nonblocking(result[0].get());
    set_nonblocking(result[1].get());
    return result;
}

Deadline deadline_from_now(std::chrono::milliseconds timeout) {
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

Deadline earlier_deadline(Deadline left, Deadline right) noexcept {
    return left < right ? left : right;
}

int poll_timeout_ms(Deadline deadline) {
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

void wait_for_io(int fd, short events, std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline,
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

void check_absolute_deadline(Deadline deadline, const char* timeout_operation) {
    if (deadline != Deadline::max() && Clock::now() >= deadline) {
        throw_system_error(timeout_operation, ETIMEDOUT);
    }
}

void write_exact(int fd, const void* data, std::size_t size, std::chrono::milliseconds inactivity_timeout,
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

void write_iovecs_exact(int fd, iovec* parts, std::size_t part_count,
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

void write_frame_with_payload(int fd, WireType type, std::uint32_t arg1, std::uint32_t arg2,
                              const void* payload, std::size_t payload_size,
                              std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    HeaderBytes header = protocol::encode_header(type, arg1, arg2);
    std::array<iovec, 2> parts{{
        {header.data(), header.size()},
        {const_cast<void*>(payload), payload_size},
    }};
    write_iovecs_exact(fd, parts.data(), parts.size(), inactivity_timeout, absolute_deadline);
}

void read_exact(int fd, void* data, std::size_t size, std::chrono::milliseconds inactivity_timeout,
                Deadline absolute_deadline) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t received = 0;

    while (received < size) {
        check_absolute_deadline(absolute_deadline, "receive timed out");
        const ssize_t result = ::recv(fd, bytes + received, size - received, 0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            throw_system_error("peer closed connection", ECONNRESET);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            wait_for_io(fd, POLLIN, inactivity_timeout, absolute_deadline, "receive timed out");
            continue;
        }
        throw_system_error("receive failed");
    }
}

void write_header(int fd, WireType type, std::uint32_t arg1, std::uint32_t arg2,
                  std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    const HeaderBytes header = protocol::encode_header(type, arg1, arg2);
    write_exact(fd, header.data(), header.size(), inactivity_timeout, absolute_deadline);
}

protocol::DecodedHeader read_header(int fd, WireType expected_type, std::chrono::milliseconds inactivity_timeout,
                                    Deadline absolute_deadline) {
    HeaderBytes header{};
    read_exact(fd, header.data(), header.size(), inactivity_timeout, absolute_deadline);
    return protocol::decode_header(header, expected_type);
}

protocol::DecodedHeader read_header(int fd, std::chrono::milliseconds inactivity_timeout,
                                    Deadline absolute_deadline) {
    HeaderBytes header{};
    read_exact(fd, header.data(), header.size(), inactivity_timeout, absolute_deadline);
    return protocol::decode_header(header);
}

void write_request(int fd, std::string_view route, std::string_view body, std::size_t max_message_size,
                   std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    protocol::validate_request_lengths(route.size(), body.size(), max_message_size);

    HeaderBytes header = protocol::encode_header(WireType::request, static_cast<std::uint32_t>(route.size()),
                                                 static_cast<std::uint32_t>(body.size()));
    std::array<iovec, 3> parts{{
        {header.data(), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact(fd, parts.data(), parts.size(), inactivity_timeout, absolute_deadline);
}

Request read_request(int fd, const protocol::DecodedHeader& header, std::size_t max_message_size,
                     std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    if (header.type != WireType::request) {
        throw std::runtime_error("unexpected protocol message type");
    }
    protocol::validate_request_lengths(header.arg1, header.arg2, max_message_size);

    Request request;
    request.route.resize(header.arg1);
    request.body.resize(header.arg2);

    read_exact(fd, request.route.data(), request.route.size(), inactivity_timeout, absolute_deadline);
    read_exact(fd, request.body.data(), request.body.size(), inactivity_timeout, absolute_deadline);
    return request;
}

class IncomingStream {
  public:
    IncomingStream(int fd, WireType chunk_type, WireType end_type, std::size_t max_size,
                   std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline)
        : fd_(fd), chunk_type_(chunk_type), end_type_(end_type), max_size_(max_size),
          inactivity_timeout_(inactivity_timeout), absolute_deadline_(absolute_deadline) {}

    std::size_t read(char* buffer, std::size_t capacity) {
        if (!active_) {
            throw std::logic_error("stream reader is no longer active");
        }
        if (capacity == 0) {
            throw std::invalid_argument("stream reader capacity must be greater than zero");
        }
        if (ended_) {
            return 0;
        }

        if (frame_remaining_ == 0) {
            const auto header = read_header(fd_, inactivity_timeout_, absolute_deadline_);
            if (header.type == end_type_) {
                if (header.arg1 != 0 || header.arg2 != 0) {
                    throw std::runtime_error("invalid stream end frame");
                }
                ended_ = true;
                return 0;
            }
            if (header.type != chunk_type_ || header.arg1 == 0 || header.arg2 != 0) {
                throw std::runtime_error("invalid stream chunk frame");
            }
            const std::size_t chunk_size = header.arg1;
            if (chunk_size > std::numeric_limits<std::size_t>::max() - total_size_ ||
                (max_size_ != 0 && chunk_size > max_size_ - total_size_)) {
                throw std::length_error("stream exceeds max_stream_size");
            }
            total_size_ += chunk_size;
            frame_remaining_ = chunk_size;
        }

        const std::size_t size = std::min(capacity, frame_remaining_);
        read_exact(fd_, buffer, size, inactivity_timeout_, absolute_deadline_);
        frame_remaining_ -= size;
        return size;
    }

    void drain(std::size_t buffer_size) {
        std::vector<char> buffer(buffer_size);
        while (read(buffer.data(), buffer.size()) != 0) {
        }
    }

    void deactivate() noexcept { active_ = false; }

  private:
    int fd_;
    WireType chunk_type_;
    WireType end_type_;
    std::size_t max_size_;
    std::chrono::milliseconds inactivity_timeout_;
    Deadline absolute_deadline_;
    std::size_t total_size_ = 0;
    std::size_t frame_remaining_ = 0;
    bool ended_ = false;
    bool active_ = true;
};

void write_stream_chunks(int fd, WireType chunk_type, const StreamReader& source, std::size_t chunk_size,
                         std::size_t max_stream_size, std::chrono::milliseconds inactivity_timeout,
                         Deadline absolute_deadline) {
    std::vector<char> buffer(chunk_size);
    std::size_t total_size = 0;

    while (source) {
        const std::size_t size = source(buffer.data(), buffer.size());
        if (size == 0) {
            break;
        }
        if (size > buffer.size()) {
            throw std::runtime_error("stream reader returned more bytes than its capacity");
        }
        if (size > std::numeric_limits<std::size_t>::max() - total_size ||
            (max_stream_size != 0 && size > max_stream_size - total_size)) {
            throw std::length_error("stream exceeds max_stream_size");
        }
        total_size += size;
        write_frame_with_payload(fd, chunk_type, static_cast<std::uint32_t>(size), 0, buffer.data(), size,
                                 inactivity_timeout, absolute_deadline);
    }
}

void write_response(int fd, const Response& response, std::size_t max_message_size,
                    std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    if (response.status_code < 0) {
        throw std::invalid_argument("response status_code must not be negative");
    }
    if (response.body.size() > max_message_size || response.body.size() > protocol::max_wire_field) {
        throw std::length_error("response exceeds max_message_size");
    }

    write_frame_with_payload(fd, WireType::response, static_cast<std::uint32_t>(response.status_code),
                             static_cast<std::uint32_t>(response.body.size()), response.body.data(),
                             response.body.size(), inactivity_timeout, absolute_deadline);
}

Response read_response(int fd, std::size_t max_message_size, std::chrono::milliseconds inactivity_timeout,
                       Deadline absolute_deadline) {
    const auto header = read_header(fd, WireType::response, inactivity_timeout, absolute_deadline);
    if (header.arg1 > static_cast<std::uint32_t>(INT_MAX)) {
        throw std::runtime_error("response status_code is out of range");
    }
    if (header.arg2 > max_message_size) {
        throw std::length_error("response exceeds max_message_size");
    }

    Response response;
    response.status_code = static_cast<int>(header.arg1);
    response.body.resize(header.arg2);
    read_exact(fd, response.body.data(), response.body.size(), inactivity_timeout, absolute_deadline);
    return response;
}

void connect_nonblocking(int fd, const sockaddr_un& address, std::chrono::milliseconds connect_timeout,
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

bool same_file_identity(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

void verify_owned_socket_candidate(const struct stat& info, const std::string& socket_path) {
    if (!S_ISSOCK(info.st_mode)) {
        throw std::runtime_error("socket path exists and is not a Unix-domain socket: " + socket_path);
    }
    if (info.st_uid != ::geteuid()) {
        throw std::runtime_error("refusing to remove a Unix-domain socket owned by another user: " + socket_path);
    }
}

std::string instance_lock_path(const std::string& socket_path) {
    return socket_path + ".lock";
}

FileDescriptor acquire_instance_lock(const std::string& socket_path) {
    const std::string lock_path = instance_lock_path(socket_path);
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif

    FileDescriptor fd(::open(lock_path.c_str(), flags, 0600));
    if (fd.get() < 0) {
        throw_system_error("open server lock file failed");
    }
    set_close_on_exec(fd.get());

    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        throw_system_error("fstat server lock file failed");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1) {
        throw std::runtime_error(
            "server lock path must be a singly-linked regular file owned by the current user: " + lock_path);
    }
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        if (error == EWOULDBLOCK || error == EAGAIN) {
            throw_system_error("socket path is already owned by another easy-uds server", EADDRINUSE);
        }
        throw_system_error("flock server lock file failed", error);
    }
    if (::fchmod(fd.get(), 0600) != 0) {
        throw_system_error("chmod server lock file failed");
    }

    return fd;
}

void remove_stale_socket(const std::string& socket_path, std::chrono::milliseconds grace_period) {
    struct stat before {};
    if (::lstat(socket_path.c_str(), &before) != 0) {
        if (errno == ENOENT) {
            return;
        }
        throw_system_error("lstat socket path failed");
    }
    verify_owned_socket_candidate(before, socket_path);

    const Deadline grace_deadline = deadline_from_now(grace_period);
    while (true) {
        FileDescriptor probe = make_socket();
        const sockaddr_un address = make_address(socket_path);
        try {
            connect_nonblocking(probe.get(), address, std::chrono::milliseconds{100}, Deadline::max());
            throw std::runtime_error("socket path is already in use: " + socket_path);
        } catch (const std::system_error& error) {
            const int connect_error = error.code().value();
            if (connect_error == ENOENT) {
                return;
            }
            if (connect_error != ECONNREFUSED) {
                throw;
            }
        }

        struct stat current {};
        if (::lstat(socket_path.c_str(), &current) != 0) {
            if (errno == ENOENT) {
                return;
            }
            throw_system_error("lstat socket path failed");
        }
        verify_owned_socket_candidate(current, socket_path);
        if (!same_file_identity(before, current)) {
            throw std::runtime_error("socket path changed while checking whether it is stale: " + socket_path);
        }

        if (grace_period.count() == 0 || Clock::now() >= grace_deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    struct stat after {};
    if (::lstat(socket_path.c_str(), &after) != 0) {
        if (errno == ENOENT) {
            return;
        }
        throw_system_error("lstat socket path failed");
    }
    verify_owned_socket_candidate(after, socket_path);
    if (!same_file_identity(before, after)) {
        throw std::runtime_error("socket path changed before stale-socket removal: " + socket_path);
    }

    if (::unlink(socket_path.c_str()) != 0 && errno != ENOENT) {
        throw_system_error("remove stale socket failed");
    }
}

void close_fd(int& fd) noexcept {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

} // namespace

namespace detail {

struct PendingConnection {
    int fd = -1;
    Deadline deadline = Deadline::max();
};

struct HandlerEntry {
    Server::Handler handler;
    bool serialized = false;
};

struct PendingSerializedRequest {
    FileDescriptor fd;
    Deadline deadline = Deadline::max();
    Request request;
    Server::Handler handler;
};

struct ServerState {
    std::string socket_path;
    ServerOptions options;

    std::atomic<bool> running{false};
    std::atomic<std::size_t> active_streams{0};
    std::size_t max_concurrent_streams = 1;

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    int listener_fd = -1;
    int wake_read_fd = -1;
    int wake_write_fd = -1;
    int instance_lock_fd = -1;
    bool stopped = false;
    bool run_started = false;
    bool run_active = false;

    std::mutex handlers_mutex;
    std::unordered_map<std::string, HandlerEntry> handlers;
    std::unordered_map<std::string, Server::StreamHandler> stream_handlers;

    std::mutex work_mutex;
    std::condition_variable work_cv;
    std::deque<PendingConnection> pending_connections;
    std::unordered_set<int> active_fds;
    bool workers_stopping = false;

    std::mutex serialized_mutex;
    std::condition_variable serialized_cv;
    std::deque<PendingSerializedRequest> pending_serialized_requests;
    std::atomic<bool> serialized_stopping{false};

    // Serialized executor thread. Lazily started on the first enqueued
    // serialized request and drained by run()/join_serialized_worker(), so a
    // server that never handles a serialized route spawns no unused thread.
    std::mutex serialized_thread_mutex;
    std::thread serialized_thread;

    std::mutex socket_path_mutex;
    dev_t socket_device{};
    ino_t socket_inode{};
    bool socket_identity_valid = false;
};

} // namespace detail

namespace {

void unlink_owned_socket(const std::shared_ptr<detail::ServerState>& state) noexcept {
    std::lock_guard<std::mutex> lock(state->socket_path_mutex);
    if (!state->socket_identity_valid) {
        return;
    }

    struct stat current {};
    if (::lstat(state->socket_path.c_str(), &current) == 0 && current.st_dev == state->socket_device &&
        current.st_ino == state->socket_inode) {
        (void)::unlink(state->socket_path.c_str());
    }
    state->socket_identity_valid = false;
}

std::string bounded_error_body(std::string_view message, std::size_t max_message_size) {
    return message.size() <= max_message_size ? std::string(message) : std::string{};
}

bool find_request_handler(const std::shared_ptr<detail::ServerState>& state, const std::string& route,
                          Server::Handler& handler, bool& serialized) {
    {
        std::lock_guard<std::mutex> lock(state->handlers_mutex);
        const auto it = state->handlers.find(route);
        if (it == state->handlers.end()) {
            return false;
        }
        handler = it->second.handler;
        serialized = it->second.serialized;
    }
    return true;
}

Response invoke_request_handler(const Server::Handler& handler, const Request& request,
                                std::size_t max_message_size) {
    try {
        return handler(request);
    } catch (...) {
        return {500, bounded_error_body("Internal Server Error", max_message_size)};
    }
}

Server::StreamHandler find_stream_handler(const std::shared_ptr<detail::ServerState>& state,
                                          const std::string& route) {
    std::lock_guard<std::mutex> lock(state->handlers_mutex);
    const auto it = state->stream_handlers.find(route);
    return it == state->stream_handlers.end() ? Server::StreamHandler{} : it->second;
}

bool try_acquire_stream_slot(const std::shared_ptr<detail::ServerState>& state) noexcept {
    std::size_t active = state->active_streams.load(std::memory_order_relaxed);
    while (active < state->max_concurrent_streams) {
        if (state->active_streams.compare_exchange_weak(active, active + 1, std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

class ActiveStreamGuard {
  public:
    explicit ActiveStreamGuard(std::shared_ptr<detail::ServerState> state) : state_(std::move(state)) {}
    ~ActiveStreamGuard() { release(); }

    ActiveStreamGuard(const ActiveStreamGuard&) = delete;
    ActiveStreamGuard& operator=(const ActiveStreamGuard&) = delete;

    void release() noexcept {
        if (state_) {
            state_->active_streams.fetch_sub(1, std::memory_order_relaxed);
            state_.reset();
        }
    }

  private:
    std::shared_ptr<detail::ServerState> state_;
};

void write_internal_server_error(int fd, const std::shared_ptr<detail::ServerState>& state, Deadline deadline) {
    write_response(fd, {500, bounded_error_body("Internal Server Error", state->options.max_message_size)},
                   state->options.max_message_size, state->options.io_timeout, deadline);
}

enum class ClientDisposition {
    complete,
    serialized_handoff,
};

bool ensure_serialized_worker(const std::shared_ptr<detail::ServerState>& state);

bool enqueue_serialized_request(const std::shared_ptr<detail::ServerState>& state, FileDescriptor client,
                                Deadline deadline, Request request, Server::Handler handler) {
    // Lazily start the serialized executor. Conditional on running so a stopped
    // server never spawns a thread; the serialized_mutex re-check below still
    // wins if stop() lands between the two steps.
    if (!ensure_serialized_worker(state)) {
        // `client`'s destructor closes the connection here, before the queue
        // ever observes the descriptor.
        return false;
    }
    std::unique_lock<std::mutex> lock(state->serialized_mutex);
    if (state->serialized_stopping.load() || !state->running.load()) {
        return false;
    }
    // Ownership of the connection moves into the queue while holding
    // serialized_mutex, the same lock stop() uses to drain queued descriptors.
    // A handed-off fd therefore always has exactly one owner at a time.
    state->pending_serialized_requests.push_back(
        {std::move(client), deadline, std::move(request), std::move(handler)});
    lock.unlock();
    state->serialized_cv.notify_one();
    return true;
}

ClientDisposition handle_client(const std::shared_ptr<detail::ServerState>& state, FileDescriptor& client,
                                Deadline deadline) noexcept {
    const int fd = client.get();
    try {
        const auto initial = read_header(fd, state->options.io_timeout, deadline);
        if (initial.type == WireType::request) {
            const Request request =
                read_request(fd, initial, state->options.max_message_size, state->options.io_timeout, deadline);

            Server::Handler handler;
            bool serialized = false;
            if (!find_request_handler(state, request.route, handler, serialized)) {
                write_response(fd, {404, bounded_error_body("Not Found", state->options.max_message_size)},
                               state->options.max_message_size, state->options.io_timeout, deadline);
                return ClientDisposition::complete;
            }

            if (serialized) {
                if (enqueue_serialized_request(state, std::move(client), deadline, request, std::move(handler))) {
                    return ClientDisposition::serialized_handoff;
                }
                return ClientDisposition::complete;
            }

            const Response response = invoke_request_handler(handler, request, state->options.max_message_size);
            try {
                write_response(fd, response, state->options.max_message_size, state->options.io_timeout, deadline);
            } catch (const std::invalid_argument&) {
                write_internal_server_error(fd, state, deadline);
            } catch (const std::length_error&) {
                write_internal_server_error(fd, state, deadline);
            }
            return ClientDisposition::complete;
        }

        if (initial.type != WireType::stream_request || initial.arg1 == 0 || initial.arg2 != 0 ||
            initial.arg1 > state->options.max_message_size) {
            throw std::runtime_error("invalid stream request header");
        }
        if (!try_acquire_stream_slot(state)) {
            return ClientDisposition::complete;
        }
        ActiveStreamGuard stream_guard(state);

        const Deadline stream_deadline = deadline_from_now(state->options.stream_timeout);
        std::string route(initial.arg1, '\0');
        read_exact(fd, route.data(), route.size(), state->options.io_timeout, stream_deadline);

        IncomingStream incoming(fd, WireType::stream_request_chunk, WireType::stream_request_end,
                                state->options.max_stream_size, state->options.io_timeout, stream_deadline);
        StreamReader reader = [&incoming](char* buffer, std::size_t capacity) {
            return incoming.read(buffer, capacity);
        };

        StreamResponse response;
        const auto handler = find_stream_handler(state, route);
        if (!handler) {
            response.status_code = 404;
        } else {
            try {
                response = handler(reader);
            } catch (...) {
                response = {500, {}};
            }
        }

        // A handler may intentionally inspect only a prefix. Consume the rest
        // before sending a response so the half-duplex exchange cannot deadlock.
        incoming.drain(state->options.stream_chunk_size);
        incoming.deactivate();

        if (response.status_code < 0) {
            response = {500, {}};
        }
        write_header(fd, WireType::stream_response, static_cast<std::uint32_t>(response.status_code), 0,
                     state->options.io_timeout, stream_deadline);
        write_stream_chunks(fd, WireType::stream_response_chunk, response.body, state->options.stream_chunk_size,
                            state->options.max_stream_size, state->options.io_timeout, stream_deadline);
        // Release admission before the end marker becomes visible to the
        // client. A sequential request can then never race the guard cleanup.
        stream_guard.release();
        write_header(fd, WireType::stream_response_end, 0, 0, state->options.io_timeout, stream_deadline);
    } catch (...) {
        // Invalid, timed-out, disconnected, or shutdown peers are isolated to
        // their worker. Public server state is unaffected.
    }
    return ClientDisposition::complete;
}

void serialized_worker_loop(const std::shared_ptr<detail::ServerState>& state) noexcept {
    while (true) {
        detail::PendingSerializedRequest job;
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            state->serialized_cv.wait(lock, [&state] {
                return state->serialized_stopping || !state->pending_serialized_requests.empty();
            });

            if (state->pending_serialized_requests.empty()) {
                if (state->serialized_stopping) {
                    return;
                }
                continue;
            }

            job = std::move(state->pending_serialized_requests.front());
            state->pending_serialized_requests.pop_front();
        }

        const int fd = job.fd.get();
        try {
            // A robot command that has already exceeded its server-side
            // absolute request deadline must never execute later just because
            // it spent time waiting behind another command.
            check_absolute_deadline(job.deadline, "serialized request timed out before execution");
            if (!state->running.load()) {
                throw_system_error("server stopped before serialized request execution", ECANCELED);
            }

            const Response response =
                invoke_request_handler(job.handler, job.request, state->options.max_message_size);
            try {
                write_response(fd, response, state->options.max_message_size,
                               state->options.io_timeout, job.deadline);
            } catch (const std::invalid_argument&) {
                write_internal_server_error(fd, state, job.deadline);
            } catch (const std::length_error&) {
                write_internal_server_error(fd, state, job.deadline);
            }
        } catch (...) {
            // Expired, disconnected, or shutdown queued requests are dropped.
            // Most importantly, the handler is not invoked after queue expiry.
        }

        // Keep the descriptor in active_fds for the entire handoff so the
        // global max_connections accounting and stop() shutdown remain valid.
        // Erase it before the job's FileDescriptor closes it (at scope exit)
        // to avoid descriptor-number reuse races.
        {
            std::lock_guard<std::mutex> lock(state->work_mutex);
            state->active_fds.erase(fd);
        }
    }
}

// Starts the serialized executor thread on first use. Returns false when the
// server is no longer running, in which case the caller must drop the
// connection without queueing it. The serialized_worker_loop is defined above,
// so this creates the thread with a complete function type.
bool ensure_serialized_worker(const std::shared_ptr<detail::ServerState>& state) {
    std::lock_guard<std::mutex> lock(state->serialized_thread_mutex);
    if (state->serialized_thread.joinable()) {
        return true;
    }
    if (!state->running.load() || state->serialized_stopping.load()) {
        return false;
    }
    state->serialized_thread = std::thread(serialized_worker_loop, state);
    return true;
}

// Moves the lazily-created serialized executor out of ServerState and joins it.
// Called by run() only after stop_state() and after every regular worker has
// been joined, so no worker can still be creating the thread at this point.
void join_serialized_worker(const std::shared_ptr<detail::ServerState>& state) noexcept {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(state->serialized_thread_mutex);
        worker = std::move(state->serialized_thread);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

void worker_loop(const std::shared_ptr<detail::ServerState>& state) noexcept {
    while (true) {
        detail::PendingConnection connection;
        {
            std::unique_lock<std::mutex> lock(state->work_mutex);
            state->work_cv.wait(lock,
                                [&state] { return state->workers_stopping || !state->pending_connections.empty(); });

            if (state->pending_connections.empty()) {
                if (state->workers_stopping) {
                    return;
                }
                continue;
            }

            connection = state->pending_connections.front();
            state->pending_connections.pop_front();
            state->active_fds.insert(connection.fd);
        }

        FileDescriptor client(connection.fd);
        const ClientDisposition disposition = handle_client(state, client, connection.deadline);

        if (disposition == ClientDisposition::serialized_handoff) {
            // Ownership was moved into the serialized queue under
            // serialized_mutex; `client` is now empty and has nothing to close.
            continue;
        }

        // Remove the descriptor from the shared set before close(). stop() uses
        // the same mutex, so it cannot act on a descriptor number after close()
        // and accidental descriptor-number reuse elsewhere in the process.
        {
            std::lock_guard<std::mutex> lock(state->work_mutex);
            state->active_fds.erase(connection.fd);
        }
    }
}

void signal_workers_to_stop(const std::shared_ptr<detail::ServerState>& state) noexcept {
    std::vector<int> pending;
    {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        state->workers_stopping = true;

        pending.reserve(state->pending_connections.size());
        for (const auto& connection : state->pending_connections) {
            pending.push_back(connection.fd);
        }
        state->pending_connections.clear();

        for (const int fd : state->active_fds) {
            (void)::shutdown(fd, SHUT_RDWR);
        }
    }

    for (const int fd : pending) {
        (void)::shutdown(fd, SHUT_RDWR);
        (void)::close(fd);
    }
    state->work_cv.notify_all();
}

void signal_serialized_worker_to_stop(const std::shared_ptr<detail::ServerState>& state) noexcept {
    std::vector<detail::PendingSerializedRequest> queued;
    {
        std::lock_guard<std::mutex> lock(state->serialized_mutex);
        state->serialized_stopping = true;
        queued.reserve(state->pending_serialized_requests.size());
        for (auto& request : state->pending_serialized_requests) {
            queued.push_back(std::move(request));
        }
        state->pending_serialized_requests.clear();
    }

    if (!queued.empty()) {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        for (const auto& request : queued) {
            state->active_fds.erase(request.fd.get());
        }
    }

    for (const auto& request : queued) {
        (void)::shutdown(request.fd.get(), SHUT_RDWR);
    }
    // `queued`'s FileDescriptors close the connections at scope exit. A fd is
    // only ever closed by the owner that holds it under serialized_mutex, so
    // stop() can never close an fd that a handing-off worker still wraps.
    state->serialized_cv.notify_all();
}

void wake_accept_loop_locked(const std::shared_ptr<detail::ServerState>& state) noexcept {
    if (state->wake_write_fd < 0) {
        return;
    }
    const unsigned char byte = 1;
    const ssize_t result = ::write(state->wake_write_fd, &byte, sizeof(byte));
    (void)result;
}

void close_lifecycle_fds_locked(const std::shared_ptr<detail::ServerState>& state) noexcept {
    close_fd(state->listener_fd);
    close_fd(state->wake_read_fd);
    close_fd(state->wake_write_fd);
    close_fd(state->instance_lock_fd);
}

void stop_state(const std::shared_ptr<detail::ServerState>& state) noexcept {
    state->running.store(false);

    bool close_without_run = false;
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        state->stopped = true;
        wake_accept_loop_locked(state);
        close_without_run = !state->run_active;
    }

    // Remove the pathname while the instance lock is still held. The inode
    // check prevents deleting a path that another actor replaced.
    unlink_owned_socket(state);

    if (close_without_run) {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (!state->run_active) {
            close_lifecycle_fds_locked(state);
        }
    }

    signal_workers_to_stop(state);
    signal_serialized_worker_to_stop(state);
}

void drain_wakeup_fd(int fd) noexcept {
    std::array<unsigned char, 64> buffer{};
    while (true) {
        const ssize_t result = ::read(fd, buffer.data(), buffer.size());
        if (result > 0) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

class RunActiveGuard {
  public:
    explicit RunActiveGuard(std::shared_ptr<detail::ServerState> state) : state_(std::move(state)) {}

    ~RunActiveGuard() {
        std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
        close_lifecycle_fds_locked(state_);
        state_->run_active = false;
        state_->lifecycle_cv.notify_all();
    }

    RunActiveGuard(const RunActiveGuard&) = delete;
    RunActiveGuard& operator=(const RunActiveGuard&) = delete;

  private:
    std::shared_ptr<detail::ServerState> state_;
};

} // namespace

Server::Server(std::string socket_path, ServerOptions options) : state_(std::make_shared<detail::ServerState>()) {
    (void)make_address(socket_path);
    validate_server_options(options);

    state_->socket_path = std::move(socket_path);
    state_->options = options;
    state_->max_concurrent_streams = std::max<std::size_t>(1, options.worker_threads - 1);

    FileDescriptor instance_lock = acquire_instance_lock(state_->socket_path);
    remove_stale_socket(state_->socket_path, options.stale_socket_grace_period);

    FileDescriptor listener = make_socket();
    const sockaddr_un address = make_address(state_->socket_path);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), address_length(address)) != 0) {
        throw_system_error("bind failed");
    }

    struct stat identity {};
    if (::lstat(state_->socket_path.c_str(), &identity) != 0) {
        const int error = errno;
        (void)::unlink(state_->socket_path.c_str());
        throw_system_error("lstat bound socket failed", error);
    }
    if (!S_ISSOCK(identity.st_mode) || identity.st_uid != ::geteuid()) {
        (void)::unlink(state_->socket_path.c_str());
        throw std::runtime_error("bound socket path was replaced before initialization completed");
    }
    state_->socket_device = identity.st_dev;
    state_->socket_inode = identity.st_ino;
    state_->socket_identity_valid = true;

    if (::chmod(state_->socket_path.c_str(), static_cast<mode_t>(options.socket_permissions)) != 0) {
        const int error = errno;
        unlink_owned_socket(state_);
        throw_system_error("chmod socket path failed", error);
    }

    if (::listen(listener.get(), options.listen_backlog) != 0) {
        const int error = errno;
        unlink_owned_socket(state_);
        throw_system_error("listen failed", error);
    }

    std::array<FileDescriptor, 2> wake_pipe;
    try {
        wake_pipe = make_wakeup_pipe();
    } catch (...) {
        unlink_owned_socket(state_);
        throw;
    }

    std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
    state_->listener_fd = listener.release();
    state_->wake_read_fd = wake_pipe[0].release();
    state_->wake_write_fd = wake_pipe[1].release();
    state_->instance_lock_fd = instance_lock.release();
}

Server::~Server() {
    const auto state = state_;
    stop_state(state);

    std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
    state->lifecycle_cv.wait(lock, [&state] { return !state->run_active; });
}

void Server::on(std::string route, Handler handler) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > state_->options.max_message_size) {
        throw std::length_error("route exceeds server max_message_size");
    }
    if (!handler) {
        throw std::invalid_argument("handler must not be empty");
    }

    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    if (!state_->handlers.emplace(std::move(route), detail::HandlerEntry{std::move(handler), false}).second) {
        throw std::runtime_error("route already exists");
    }
}

void Server::on_serialized(std::string route, Handler handler) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > state_->options.max_message_size) {
        throw std::length_error("route exceeds server max_message_size");
    }
    if (!handler) {
        throw std::invalid_argument("handler must not be empty");
    }

    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    if (!state_->handlers.emplace(std::move(route), detail::HandlerEntry{std::move(handler), true}).second) {
        throw std::runtime_error("route already exists");
    }
}

void Server::on_stream(std::string route, StreamHandler handler) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > state_->options.max_message_size || route.size() > protocol::max_wire_field) {
        throw std::length_error("route exceeds server max_message_size");
    }
    if (!handler) {
        throw std::invalid_argument("stream handler must not be empty");
    }

    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    if (!state_->stream_handlers.emplace(std::move(route), std::move(handler)).second) {
        throw std::runtime_error("stream route already exists");
    }
}

void Server::set_max_concurrent_streams(std::size_t limit) {
    if (limit == 0 || limit > state_->options.worker_threads) {
        throw std::invalid_argument("max_concurrent_streams must be between 1 and worker_threads");
    }

    std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
    if (state_->run_started || state_->stopped) {
        throw std::logic_error("max_concurrent_streams must be configured before run()");
    }
    state_->max_concurrent_streams = limit;
}

void Server::run() {
    const auto state = state_;

    int listener = -1;
    int wake_read = -1;
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (state->run_started) {
            throw std::logic_error("server run() can only be called once");
        }
        state->run_started = true;
        if (state->stopped || state->listener_fd < 0 || state->wake_read_fd < 0) {
            throw std::logic_error("server has already been stopped");
        }
        state->run_active = true;
        state->running.store(true);
        listener = state->listener_fd;
        wake_read = state->wake_read_fd;
    }

    RunActiveGuard active_guard(state);

    std::vector<std::thread> workers;
    workers.reserve(state->options.worker_threads);

    try {
        for (std::size_t index = 0; index < state->options.worker_threads; ++index) {
            workers.emplace_back(worker_loop, state);
        }
    } catch (...) {
        stop_state(state);
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        join_serialized_worker(state);
        throw;
    }

    std::exception_ptr accept_error;

    while (state->running.load()) {
        std::array<pollfd, 2> items{};
        items[0].fd = listener;
        items[0].events = POLLIN;
        items[1].fd = wake_read;
        items[1].events = POLLIN;

        const int poll_result = ::poll(items.data(), static_cast<nfds_t>(items.size()), -1);
        if (poll_result < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            if (!state->running.load()) {
                break;
            }
            accept_error =
                std::make_exception_ptr(std::system_error(error, std::generic_category(), "accept poll failed"));
            break;
        }

        if ((items[1].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
            drain_wakeup_fd(wake_read);
            break;
        }

        if ((items[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            if (!state->running.load()) {
                break;
            }
            int socket_error = 0;
            socklen_t length = sizeof(socket_error);
            if (::getsockopt(listener, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
                socket_error = errno;
            }
            if (socket_error == 0) {
                socket_error = EIO;
            }
            accept_error =
                std::make_exception_ptr(std::system_error(socket_error, std::generic_category(), "listening socket failed"));
            break;
        }

        if ((items[0].revents & POLLIN) == 0) {
            continue;
        }

        // Drain a bounded batch after one readiness event. The bound avoids a
        // poll() syscall per connection during bursts while ensuring that a
        // continuously busy listener regularly rechecks the wakeup descriptor.
        std::size_t accepted_in_batch = 0;
        while (state->running.load() && accepted_in_batch < max_accept_batch) {
#if defined(__linux__) && defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
            const int client_fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
            const int client_fd = ::accept(listener, nullptr, nullptr);
#endif
            if (client_fd < 0) {
                const int error = errno;
                if (error == EINTR || error == ECONNABORTED) {
                    continue;
                }
                if (error == EAGAIN || error == EWOULDBLOCK) {
                    break;
                }
                if (!state->running.load()) {
                    break;
                }
                accept_error =
                    std::make_exception_ptr(std::system_error(error, std::generic_category(), "accept failed"));
                break;
            }
            ++accepted_in_batch;

            FileDescriptor client(client_fd);
            try {
#if !defined(__linux__) || !defined(SOCK_CLOEXEC) || !defined(SOCK_NONBLOCK)
                set_close_on_exec(client.get());
                set_nonblocking(client.get());
#endif
                configure_no_sigpipe(client.get());
            } catch (...) {
                continue;
            }

            bool accepted = false;
            {
                std::lock_guard<std::mutex> lock(state->work_mutex);
                if (!state->workers_stopping && state->running.load() &&
                    state->pending_connections.size() + state->active_fds.size() < state->options.max_connections) {
                    state->pending_connections.push_back(
                        {client.release(), deadline_from_now(state->options.request_timeout)});
                    accepted = true;
                }
            }
            if (accepted) {
                state->work_cv.notify_one();
            }
        }
        if (accept_error) {
            break;
        }
    }

    stop_state(state);
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    join_serialized_worker(state);

    if (accept_error) {
        std::rethrow_exception(accept_error);
    }
}

void Server::stop() noexcept {
    stop_state(state_);
}

bool Server::is_running() const noexcept {
    return state_->running.load();
}

const std::string& Server::socket_path() const noexcept {
    return state_->socket_path;
}

Client::Client(std::string socket_path, ClientOptions options)
    : socket_path_(std::move(socket_path)), options_(options) {
    (void)make_address(socket_path_);
    validate_client_options(options_);
}

Response Client::request(std::string_view route, std::string_view body) const {
    protocol::validate_request_lengths(route.size(), body.size(), options_.max_message_size);

    const Deadline deadline = deadline_from_now(options_.request_timeout);
    FileDescriptor fd = make_socket();
    const sockaddr_un address = make_address(socket_path_);
    connect_nonblocking(fd.get(), address, options_.connect_timeout, deadline);

    write_request(fd.get(), route, body, options_.max_message_size, options_.io_timeout, deadline);
    return read_response(fd.get(), options_.max_message_size, options_.io_timeout, deadline);
}

int Client::request_stream(std::string_view route, const StreamReader& request_body,
                           const std::function<void(std::string_view)>& response_chunk) const {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > options_.max_message_size || route.size() > protocol::max_wire_field) {
        throw std::length_error("route exceeds max_message_size");
    }

    const Deadline deadline = deadline_from_now(options_.stream_timeout);
    FileDescriptor fd = make_socket();
    const sockaddr_un address = make_address(socket_path_);
    connect_nonblocking(fd.get(), address, options_.connect_timeout, deadline);

    write_frame_with_payload(fd.get(), WireType::stream_request, static_cast<std::uint32_t>(route.size()), 0,
                             route.data(), route.size(), options_.io_timeout, deadline);
    write_stream_chunks(fd.get(), WireType::stream_request_chunk, request_body, options_.stream_chunk_size,
                        options_.max_stream_size, options_.io_timeout, deadline);
    write_header(fd.get(), WireType::stream_request_end, 0, 0, options_.io_timeout, deadline);

    const auto header = read_header(fd.get(), WireType::stream_response, options_.io_timeout, deadline);
    if (header.arg1 > static_cast<std::uint32_t>(INT_MAX) || header.arg2 != 0) {
        throw std::runtime_error("invalid stream response header");
    }

    IncomingStream incoming(fd.get(), WireType::stream_response_chunk, WireType::stream_response_end,
                            options_.max_stream_size, options_.io_timeout, deadline);
    std::vector<char> buffer(options_.stream_chunk_size);
    while (true) {
        const std::size_t size = incoming.read(buffer.data(), buffer.size());
        if (size == 0) {
            break;
        }
        if (response_chunk) {
            response_chunk(std::string_view(buffer.data(), size));
        }
    }
    incoming.deactivate();
    return static_cast<int>(header.arg1);
}

} // namespace easy_uds
