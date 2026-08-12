#include "core.hpp"

#include <algorithm>
#include <limits>

#include <unistd.h>

namespace easy_uds::detail {
namespace {

constexpr std::size_t kMaxInflightRequests = 64;
constexpr std::size_t kResumeInflightRequests = kMaxInflightRequests / 2;
constexpr std::size_t kMinimumInflightBytes = 4U * 1024U * 1024U;

std::size_t inflight_byte_limit(const std::shared_ptr<ServerState>& state) noexcept {
    return std::max(kMinimumInflightBytes, state->options.max_message_size);
}

bool below_resume_watermark(const std::shared_ptr<ServerState>& state,
                            const std::shared_ptr<Connection>& connection) noexcept {
    const bool global_below = state->options.max_total_inflight_bytes == 0 ||
        state->total_inflight_request_bytes.load(std::memory_order_acquire) <=
            state->options.max_total_inflight_bytes / 2;
    return global_below &&
           connection->inflight_requests.load(std::memory_order_acquire) <= kResumeInflightRequests &&
           connection->inflight_request_bytes.load(std::memory_order_acquire) <=
               inflight_byte_limit(state) / 2;
}

bool above_pause_watermark(const std::shared_ptr<ServerState>& state,
                           const std::shared_ptr<Connection>& connection) noexcept {
    const bool global_above = state->options.max_total_inflight_bytes != 0 &&
        state->total_inflight_request_bytes.load(std::memory_order_acquire) >=
            state->options.max_total_inflight_bytes;
    return global_above ||
           connection->inflight_requests.load(std::memory_order_acquire) >= kMaxInflightRequests ||
           connection->inflight_request_bytes.load(std::memory_order_acquire) >= inflight_byte_limit(state);
}

void wake_reactor(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->wake_write_fd < 0) {
        return;
    }
    const unsigned char byte = 1;
    const ssize_t ignored = ::write(state->wake_write_fd, &byte, sizeof(byte));
    (void)ignored;
}

} // namespace

void account_connection_request(const std::shared_ptr<ServerState>& state,
                                const std::shared_ptr<Connection>& connection,
                                std::size_t request_bytes) noexcept {
    connection->inflight_requests.fetch_add(1, std::memory_order_relaxed);
    connection->inflight_request_bytes.fetch_add(request_bytes, std::memory_order_relaxed);
    state->total_inflight_request_bytes.fetch_add(request_bytes, std::memory_order_relaxed);
}

void release_connection_request(const std::shared_ptr<ServerState>& state,
                                const std::shared_ptr<Connection>& connection,
                                std::size_t request_bytes) noexcept {
    connection->inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_release);
    connection->inflight_requests.fetch_sub(1, std::memory_order_acq_rel);
    state->total_inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_release);

    bool resumed = false;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        const auto it = state->connections.find(connection->fd);
        if (it != state->connections.end() && it->second->conn == connection && it->second->read_paused &&
            !connection->closing.load(std::memory_order_acquire) && below_resume_watermark(state, connection)) {
            it->second->read_paused = false;
            state->resumed_connections.push_back(it->second);
            resumed = true;
        }
    }
    if (resumed) {
        (void)refresh_connection_events(state, connection);
        wake_reactor(state);
    }
}

bool pause_connection_reads_if_needed(const std::shared_ptr<ServerState>& state,
                                      const std::shared_ptr<ReactorConnection>& connection) noexcept {
    const auto& peer = connection->conn;
    if (!above_pause_watermark(state, peer)) {
        return false;
    }

    bool paused = false;
    bool should_stop_reading = false;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        const auto it = state->connections.find(peer->fd);
        if (it != state->connections.end() && it->second == connection &&
            above_pause_watermark(state, peer)) {
            should_stop_reading = true;
            if (!connection->read_paused) {
                connection->read_paused = true;
                paused = true;
            }
        }
    }
    if (paused && !refresh_connection_events(state, peer)) {
        peer->closing.store(true, std::memory_order_release);
    }
    return should_stop_reading;
}

} // namespace easy_uds::detail
