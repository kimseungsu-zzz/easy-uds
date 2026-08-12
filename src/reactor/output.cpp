#include "core.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string_view>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace easy_uds::detail {
namespace {

constexpr std::size_t kMaxFlushBytes = 256U * 1024U;

std::uint64_t connection_token(int fd, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint32_t>(fd);
}

void mark_output_progress(const std::shared_ptr<Connection>& connection) noexcept {
    const auto now = Clock::now().time_since_epoch().count();
    connection->last_io_progress.store(now, std::memory_order_relaxed);
    connection->last_output_progress.store(now, std::memory_order_relaxed);
}

void wake_reactor(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->wake_write_fd < 0) {
        return;
    }
    const std::uint64_t increment = 1;
    const ssize_t ignored = ::write(state->wake_write_fd, &increment, sizeof(increment));
    (void)ignored;
}

std::size_t output_queue_limit(const std::shared_ptr<ServerState>& state) noexcept {
    return state->options.max_output_bytes_per_connection;
}

bool reserve_global_output(const std::shared_ptr<ServerState>& state, std::size_t bytes) noexcept {
    const std::size_t limit = state->options.max_total_output_bytes;
    if (limit == 0) {
        state->total_queued_output_bytes.fetch_add(bytes, std::memory_order_relaxed);
        return true;
    }
    std::size_t current = state->total_queued_output_bytes.load(std::memory_order_relaxed);
    while (current <= limit && bytes <= limit - current) {
        if (state->total_queued_output_bytes.compare_exchange_weak(
                current, current + bytes, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

std::array<iovec, 2> remaining_parts(OutgoingFrame& frame, std::size_t& part_count) noexcept {
    std::array<iovec, 2> parts{};
    if (frame.offset < frame.header.size()) {
        parts[0].iov_base = frame.header.data() + frame.offset;
        parts[0].iov_len = frame.header.size() - frame.offset;
        parts[1].iov_base = frame.body.data();
        parts[1].iov_len = frame.body.size();
        part_count = frame.body.empty() ? 1 : 2;
    } else {
        const std::size_t body_offset = frame.offset - frame.header.size();
        parts[0].iov_base = frame.body.data() + body_offset;
        parts[0].iov_len = frame.body.size() - body_offset;
        part_count = 1;
    }
    return parts;
}

ssize_t send_frame_once(int fd, OutgoingFrame& frame) {
    std::size_t part_count = 0;
    auto parts = remaining_parts(frame, part_count);
    msghdr message{};
    message.msg_iov = parts.data();
    message.msg_iovlen = part_count;
    while (true) {
#ifdef MSG_NOSIGNAL
        const ssize_t result = ::sendmsg(fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
#else
        const ssize_t result = ::sendmsg(fd, &message, MSG_DONTWAIT);
#endif
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return result;
    }
}

Deadline inactivity_deadline(const std::shared_ptr<Connection>& connection,
                             std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() == 0) {
        return Deadline::max();
    }
    const Deadline progress{Clock::duration{
        connection->last_output_progress.load(std::memory_order_relaxed)}};
    const auto max_remaining = Deadline::max() - progress;
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_remaining);
    return timeout >= max_ms ? Deadline::max() - Clock::duration{1} : progress + timeout;
}

} // namespace

bool refresh_connection_events(const std::shared_ptr<ServerState>& state,
                               const std::shared_ptr<Connection>& connection) noexcept {
    std::lock_guard<std::mutex> lock(state->connections_mutex);
    const auto it = state->connections.find(connection->fd);
    if (it == state->connections.end() || it->second->conn != connection || state->epoll_fd < 0 ||
        connection->closing.load(std::memory_order_acquire) ||
        connection->stream_active.load(std::memory_order_acquire) ||
        connection->worker_owned.load(std::memory_order_acquire)) {
        return false;
    }
    epoll_event event{};
    if (!it->second->read_paused) {
        event.events |= EPOLLIN;
    }
    if (connection->queued_output_bytes.load(std::memory_order_acquire) != 0) {
        event.events |= EPOLLOUT;
    }
    event.data.u64 = connection_token(connection->fd, it->second->generation);
    if (::epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD, connection->fd, &event) != 0) {
        connection->closing.store(true, std::memory_order_release);
        (void)::shutdown(connection->fd, SHUT_RDWR);
        return false;
    }
    return true;
}

void write_error_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          std::string_view message, std::chrono::milliseconds io_timeout, Deadline deadline,
                          easy_uds::Status status) {
    write_fixed_response(state, connection, request_id,
                         {status, bounded_error_body(message, state->options.max_message_size)},
                         io_timeout, deadline);
}

void write_fixed_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          easy_uds::Response response, std::chrono::milliseconds io_timeout,
                          Deadline deadline) {
    (void)io_timeout;
    if (response.status < 0) {
        write_error_response(state, connection, request_id,
                             state->options.include_handler_error_messages
                                 ? "response status_code must not be negative"
                                 : "Internal Server Error",
                             state->options.io_timeout, deadline);
        return;
    }
    if (response.body.size() > state->options.max_message_size ||
        response.body.size() > protocol::max_wire_field) {
        write_error_response(state, connection, request_id,
                             state->options.include_handler_error_messages ? "response exceeds max_message_size"
                                                                           : "Internal Server Error",
                             state->options.io_timeout, deadline);
        return;
    }
    check_absolute_deadline(deadline, "send timed out");

    OutgoingFrame frame;
    frame.header = protocol::encode_header(WireType::response, request_id,
                                           static_cast<std::uint32_t>(response.status),
                                           static_cast<std::uint32_t>(response.body.size()));
    frame.body = std::move(response.body);
    frame.deadline = deadline;
    const std::size_t frame_size = frame.header.size() + frame.body.size();
    bool queued = false;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(connection->output_mutex);
        std::size_t pending = connection->queued_output_bytes.load(std::memory_order_relaxed);
        if (connection->output_queue.empty()) {
            const ssize_t sent = send_frame_once(connection->fd, frame);
            if (sent > 0) {
                frame.offset = static_cast<std::size_t>(sent);
                mark_output_progress(connection);
                if (frame.offset == frame_size) {
                    return;
                }
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                connection->closing.store(true, std::memory_order_release);
                throw_system_error("send failed");
            }
        }

        const std::size_t remaining = frame_size - frame.offset;
        const std::size_t limit = output_queue_limit(state);
        bool reserved_global = false;
        if (remaining <= limit && pending <= limit - remaining) {
            reserved_global = reserve_global_output(state, remaining);
        }
        if (!reserved_global) {
            overflow = true;
        } else {
            if (pending == 0) {
                connection->last_output_progress.store(Clock::now().time_since_epoch().count(),
                                                       std::memory_order_relaxed);
            }
            connection->output_queue.push_back(std::move(frame));
            pending += remaining;
            connection->queued_output_bytes.store(pending, std::memory_order_release);
            queued = true;
        }
    }

    if (overflow) {
        connection->closing.store(true, std::memory_order_release);
        close_connection(state, connection->fd);
        throw std::runtime_error("connection output queue limit exceeded");
    }
    if (queued) {
        if (!refresh_connection_events(state, connection)) {
            throw std::runtime_error("failed to enable connection output");
        }
        wake_reactor(state);
    }
}

bool flush_connection_output(const std::shared_ptr<ServerState>& state,
                             const std::shared_ptr<ReactorConnection>& reactor_connection) {
    const auto& connection = reactor_connection->conn;
    std::size_t flushed = 0;
    bool empty = false;
    {
        std::lock_guard<std::mutex> lock(connection->output_mutex);
        while (!connection->output_queue.empty() && flushed < kMaxFlushBytes) {
            auto& frame = connection->output_queue.front();
            if (frame.deadline != Deadline::max() && Clock::now() >= frame.deadline) {
                connection->closing.store(true, std::memory_order_release);
                return false;
            }
            const ssize_t sent = send_frame_once(connection->fd, frame);
            if (sent > 0) {
                const std::size_t count = static_cast<std::size_t>(sent);
                frame.offset += count;
                flushed += count;
                connection->queued_output_bytes.fetch_sub(count, std::memory_order_acq_rel);
                state->total_queued_output_bytes.fetch_sub(count, std::memory_order_acq_rel);
                mark_output_progress(connection);
                if (frame.offset == frame.header.size() + frame.body.size()) {
                    connection->output_queue.pop_front();
                }
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            connection->closing.store(true, std::memory_order_release);
            return false;
        }
        empty = connection->output_queue.empty();
    }
    if (empty && !refresh_connection_events(state, connection) &&
        !connection->closing.load(std::memory_order_acquire)) {
        return false;
    }
    return true;
}

Deadline connection_output_deadline(const std::shared_ptr<ServerState>& state,
                                    const std::shared_ptr<Connection>& connection) {
    if (connection->queued_output_bytes.load(std::memory_order_acquire) == 0) {
        return Deadline::max();
    }
    std::lock_guard<std::mutex> lock(connection->output_mutex);
    if (connection->output_queue.empty()) {
        return Deadline::max();
    }
    Deadline deadline = inactivity_deadline(connection, state->options.io_timeout);
    for (const auto& frame : connection->output_queue) {
        deadline = earlier_deadline(deadline, frame.deadline);
    }
    return deadline;
}

} // namespace easy_uds::detail
