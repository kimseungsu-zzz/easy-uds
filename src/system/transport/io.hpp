#pragma once

// Shared implementation infrastructure for the reactor server and the client:
// Deadlines, non-blocking read-ahead/exact I/O, and transport framing. Raw
// descriptor lifecycle and socket syscalls live in platform capabilities.

#include "easy_uds/error.hpp"
#include "easy_uds/options.hpp"
#include "easy_uds/request.hpp"
#include "../protocol/codec.hpp"
#include "../platform/descriptor_passing.hpp"
#include "../platform/socket_io.hpp"
#include "../platform/socket_lifecycle.hpp"
#include "../platform/socket_wait.hpp"
#include "../platform/endpoint.hpp"

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

namespace easy_uds::detail {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
using protocol::HeaderBytes;
using protocol::WireType;

[[noreturn]] inline void throw_system_error(const char* operation, int error = errno) {
    const std::error_code system_code(error, std::generic_category());
    throw easy_uds::Error(easy_uds::detail::classify_system_error(system_code),
                          operation, system_code);
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
    if (options.stats != easy_uds::StatsMode::disabled &&
        options.stats != easy_uds::StatsMode::basic) {
        throw std::invalid_argument("stats must be StatsMode::disabled or StatsMode::basic");
    }
    if (options.max_connections == 0) {
        throw std::invalid_argument("max_connections must be greater than zero");
    }
    if (options.max_inflight_requests_per_connection == 0) {
        throw std::invalid_argument("max_inflight_requests_per_connection must be greater than zero");
    }
    if (options.max_inflight_request_bytes_per_connection < options.max_message_size) {
        throw std::invalid_argument(
            "max_inflight_request_bytes_per_connection must be at least max_message_size");
    }
    if (options.max_message_size > std::numeric_limits<std::size_t>::max() - protocol::header_size ||
        options.max_output_bytes_per_connection < options.max_message_size + protocol::header_size) {
        throw std::invalid_argument(
            "max_output_bytes_per_connection must fit one maximum response frame");
    }
    if (options.max_total_inflight_bytes != 0 &&
        options.max_total_inflight_bytes < options.max_message_size) {
        throw std::invalid_argument("max_total_inflight_bytes must be zero or at least max_message_size");
    }
    if (options.max_total_output_bytes != 0 &&
        (options.max_message_size > protocol::max_wire_field - protocol::header_size ||
         options.max_total_output_bytes < options.max_message_size + protocol::header_size)) {
        throw std::invalid_argument(
            "max_total_output_bytes must be zero or large enough for one maximum response frame");
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
    if (options.stats != easy_uds::StatsMode::disabled &&
        options.stats != easy_uds::StatsMode::basic) {
        throw std::invalid_argument("stats must be StatsMode::disabled or StatsMode::basic");
    }
}

inline void set_close_on_exec(NativeSocket fd) {
    const auto result = socket_lifecycle::set_close_on_exec(fd);
    if (!result.ok()) {
        const char* operation = result.failure == socket_lifecycle::SetupFailure::close_on_exec_getfd
                                    ? "fcntl(F_GETFD) failed"
                                    : "fcntl(F_SETFD) failed";
        throw_system_error(operation, result.native_error);
    }
}

inline void set_nonblocking(NativeSocket fd) {
    const auto result = socket_lifecycle::set_nonblocking(fd);
    if (!result.ok()) {
        const char* operation = result.failure == socket_lifecycle::SetupFailure::nonblocking_getfl
                                    ? "fcntl(F_GETFL) failed"
                                    : "fcntl(F_SETFL O_NONBLOCK) failed";
        throw_system_error(operation, result.native_error);
    }
}

inline void configure_no_sigpipe(NativeSocket fd) {
    const auto result = socket_lifecycle::configure_no_sigpipe(fd);
    if (!result.ok()) {
        throw_system_error("setsockopt(SO_NOSIGPIPE) failed", result.native_error);
    }
}

inline FileDescriptor make_socket() {
    FileDescriptor fd(platform::create_stream_socket());
    if (fd.get() < 0) {
        throw_system_error("socket failed");
    }
    // Linux creates sockets with O_NONBLOCK in the platform call; Windows
    // requires the equivalent setup explicitly. Keeping this at the cold
    // socket-creation boundary makes listener and client behavior identical.
    set_nonblocking(fd.get());
    configure_no_sigpipe(fd.get());
    return fd;
}

inline platform::UnixEndpoint make_address(const std::string& socket_path) {
    return platform::make_endpoint(socket_path);
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

inline Deadline deadline_from(Clock::time_point start,
                              std::chrono::milliseconds timeout) {
    if (timeout.count() == 0) {
        return Deadline::max();
    }
    const auto max_remaining = Deadline::max() - start;
    const auto max_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(max_remaining);
    if (timeout >= max_ms) {
        return Deadline::max() - Clock::duration{1};
    }
    return start + timeout;
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

inline void wait_for_io(NativeSocket fd, socket_wait::Interest interest,
                        std::chrono::milliseconds inactivity_timeout,
                        Deadline absolute_deadline,
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

        const auto result = socket_wait::wait_once(fd, interest, timeout_ms);
        switch (result.status) {
        case socket_wait::Status::ready:
            return;
        case socket_wait::Status::timed_out:
            throw_system_error(timeout_operation, ETIMEDOUT);
        case socket_wait::Status::interrupted:
            continue;
        case socket_wait::Status::invalid_descriptor:
            throw_system_error("socket descriptor became invalid",
                               result.native_error == 0 ? EBADF : result.native_error);
        case socket_wait::Status::error:
            throw_system_error("poll failed",
                               result.native_error == 0 ? EIO : result.native_error);
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
    explicit BufferedReader(NativeSocket fd) noexcept : fd_(fd) {}

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
            const ssize_t result = socket_io::receive(fd_, data, capacity);
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
                wait_for_io(fd_, socket_wait::Interest::read, inactivity_timeout,
                            absolute_deadline, "receive timed out");
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

    NativeSocket fd_;
    std::array<char, read_ahead_capacity> buffer_{};
    std::size_t start_ = 0;
    std::size_t size_ = 0;
};

inline void write_exact(NativeSocket fd, const void* data, std::size_t size, std::chrono::milliseconds inactivity_timeout,
                        Deadline absolute_deadline) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;

    while (sent < size) {
        check_absolute_deadline(absolute_deadline, "send timed out");
        const ssize_t result = socket_io::send(fd, bytes + sent, size - sent);
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
            wait_for_io(fd, socket_wait::Interest::write, inactivity_timeout,
                        absolute_deadline, "send timed out");
            continue;
        }
        throw_system_error("send failed");
    }
}

inline void write_iovecs_exact(NativeSocket fd, iovec* parts, std::size_t part_count,
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
        const ssize_t result = socket_io::send_iovecs(fd, parts + first,
                                                      part_count - first);
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
            wait_for_io(fd, socket_wait::Interest::write, inactivity_timeout,
                        absolute_deadline, "send timed out");
            continue;
        }
        throw_system_error("send failed");
    }
}

// Variant of write_iovecs_exact that attaches descriptor ancillary data on the
// FIRST sendmsg only, so partial-send retries never duplicate the descriptor
// (the kernel delivers it with the bytes of that first successful write).
inline void write_iovecs_exact_with_fd(NativeSocket fd, iovec* parts, std::size_t part_count, NativeSocket passed_fd,
                                       std::chrono::milliseconds inactivity_timeout,
                                       Deadline absolute_deadline) {
    std::size_t first = 0;
    bool fd_attached = false;
    while (first < part_count) {
        while (first < part_count && parts[first].iov_len == 0) {
            ++first;
        }
        if (first == part_count) {
            return;
        }

        check_absolute_deadline(absolute_deadline, "send timed out");
        const ssize_t result = descriptor_passing::send_iovecs(
            fd, parts + first, part_count - first, passed_fd, !fd_attached);
        if (result > 0) {
            fd_attached = true;
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
            wait_for_io(fd, socket_wait::Interest::write, inactivity_timeout,
                        absolute_deadline, "send timed out");
            continue;
        }
        throw_system_error("send failed");
    }
}

inline void write_frame_with_payload(NativeSocket fd, WireType type, std::uint32_t request_id, std::uint32_t arg1,
                                     std::uint32_t arg2, const void* payload, std::size_t payload_size,
                                     std::chrono::milliseconds inactivity_timeout, Deadline absolute_deadline) {
    HeaderBytes header = protocol::encode_header(type, request_id, arg1, arg2);
    std::array<iovec, 2> parts{{
        {header.data(), header.size()},
        {const_cast<void*>(payload), payload_size},
    }};
    write_iovecs_exact(fd, parts.data(), parts.size(), inactivity_timeout, absolute_deadline);
}

inline void write_header_frame(NativeSocket fd, WireType type, std::uint32_t request_id, std::uint32_t arg1,
                               std::uint32_t arg2, std::chrono::milliseconds inactivity_timeout,
                               Deadline absolute_deadline) {
    const HeaderBytes header = protocol::encode_header(type, request_id, arg1, arg2);
    write_exact(fd, header.data(), header.size(), inactivity_timeout, absolute_deadline);
}

inline void connect_nonblocking(NativeSocket fd, const platform::UnixEndpoint& address,
                                std::chrono::milliseconds connect_timeout,
                                Deadline request_deadline) {
    if (platform::connect_socket(fd, address) == 0) {
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

    wait_for_io(fd, socket_wait::Interest::write, std::chrono::milliseconds{0},
                deadline, "connect timed out");

    int socket_error = 0;
    if (socket_io::query_socket_error(fd, socket_error) != 0) {
        throw_system_error("getsockopt(SO_ERROR) failed");
    }
    if (socket_error != 0) {
        throw_system_error("connect failed", socket_error);
    }
}

} // namespace easy_uds::detail
