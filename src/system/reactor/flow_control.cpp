#include "core.hpp"

#include <algorithm>
#include <limits>
#include <vector>

#include <unistd.h>

namespace easy_uds::detail {
namespace {

std::size_t inflight_byte_limit(const std::shared_ptr<ServerState>& state) noexcept {
    return state->options.max_inflight_request_bytes_per_connection;
}

bool below_resume_watermark(const std::shared_ptr<ServerState>& state,
                            const std::shared_ptr<Connection>& connection) noexcept {
    const bool global_below = state->options.max_total_inflight_bytes == 0 ||
        state->total_inflight_request_bytes.load(std::memory_order_acquire) <=
            state->options.max_total_inflight_bytes / 2;
    return global_below &&
           connection->inflight_requests.load(std::memory_order_acquire) <=
               state->options.max_inflight_requests_per_connection / 2 &&
           connection->inflight_request_bytes.load(std::memory_order_acquire) <=
               inflight_byte_limit(state) / 2;
}

bool above_pause_watermark(const std::shared_ptr<ServerState>& state,
                           const std::shared_ptr<Connection>& connection) noexcept {
    const bool global_above = state->options.max_total_inflight_bytes != 0 &&
        state->total_inflight_request_bytes.load(std::memory_order_acquire) >=
            state->options.max_total_inflight_bytes;
    return global_above ||
           connection->inflight_requests.load(std::memory_order_acquire) >=
               state->options.max_inflight_requests_per_connection ||
           connection->inflight_request_bytes.load(std::memory_order_acquire) >= inflight_byte_limit(state);
}

void wake_reactor(const std::shared_ptr<ServerState>& state) noexcept;

void resume_paused_connections(const std::shared_ptr<ServerState>& state,
                               const std::shared_ptr<Connection>& released) noexcept {
    std::vector<std::shared_ptr<Connection>> refresh;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        auto queue_if_ready = [&](const std::shared_ptr<ReactorConnection>& reactor_connection) {
            const auto& connection = reactor_connection->conn;
            if (!reactor_connection->read_paused ||
                connection->closing.load(std::memory_order_acquire) ||
                !below_resume_watermark(state, connection)) {
                return;
            }
            reactor_connection->read_paused = false;
            state->resumed_connections.push_back(reactor_connection);
            refresh.push_back(connection);
        };

        if (state->options.max_total_inflight_bytes == 0) {
            const auto it = state->connections.find(released->fd);
            if (it != state->connections.end() && it->second->conn == released) {
                queue_if_ready(it->second);
            }
        } else {
            for (const auto& entry : state->connections) {
                queue_if_ready(entry.second);
            }
        }
    }
    for (const auto& connection : refresh) {
        (void)refresh_connection_events(state, connection);
    }
    if (!refresh.empty()) {
        wake_reactor(state);
    }
}

void wake_reactor(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->wake_write_fd < 0) {
        return;
    }
    bool expected = false;
    if (!state->wake_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t increment = 1;
    const ssize_t ignored = ::write(state->wake_write_fd, &increment, sizeof(increment));
    (void)ignored;
}

} // namespace

bool try_reserve_connection_request_bytes(const std::shared_ptr<ServerState>& state,
                                          const std::shared_ptr<Connection>& connection,
                                          std::size_t request_bytes) noexcept {
    if (request_bytes == 0) {
        return true;
    }

    const std::size_t connection_limit = inflight_byte_limit(state);
    std::size_t connection_bytes =
        connection->inflight_request_bytes.load(std::memory_order_acquire);
    while (request_bytes <= connection_limit - std::min(connection_bytes, connection_limit)) {
        if (connection->inflight_request_bytes.compare_exchange_weak(
                connection_bytes, connection_bytes + request_bytes,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
    if (connection_bytes > connection_limit || request_bytes > connection_limit - connection_bytes) {
        return false;
    }

    const std::size_t global_limit = state->options.max_total_inflight_bytes;
    std::size_t global_bytes =
        state->total_inflight_request_bytes.load(std::memory_order_acquire);
    while (global_bytes <= global_limit && request_bytes <= global_limit - global_bytes) {
        if (state->total_inflight_request_bytes.compare_exchange_weak(
                global_bytes, global_bytes + request_bytes,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }

    connection->inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_acq_rel);
    return false;
}

void release_connection_request_bytes(const std::shared_ptr<ServerState>& state,
                                      const std::shared_ptr<Connection>& connection,
                                      std::size_t request_bytes) noexcept {
    if (request_bytes == 0) {
        return;
    }
    connection->inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_acq_rel);
    state->total_inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_acq_rel);
    resume_paused_connections(state, connection);
}

void account_connection_request(const std::shared_ptr<ServerState>& state,
                                const std::shared_ptr<Connection>& connection,
                                std::size_t request_bytes) noexcept {
    connection->inflight_requests.fetch_add(1, std::memory_order_relaxed);
    connection->inflight_request_bytes.fetch_add(request_bytes, std::memory_order_relaxed);
    state->total_inflight_request_bytes.fetch_add(request_bytes, std::memory_order_relaxed);
}

void account_connection_request_count(const std::shared_ptr<Connection>& connection) noexcept {
    connection->inflight_requests.fetch_add(1, std::memory_order_relaxed);
}

void release_connection_request(const std::shared_ptr<ServerState>& state,
                                const std::shared_ptr<Connection>& connection,
                                std::size_t request_bytes) noexcept {
    connection->inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_release);
    connection->inflight_requests.fetch_sub(1, std::memory_order_acq_rel);
    state->total_inflight_request_bytes.fetch_sub(request_bytes, std::memory_order_release);

    resume_paused_connections(state, connection);
}

void pause_connection_reads(const std::shared_ptr<ServerState>& state,
                            const std::shared_ptr<ReactorConnection>& connection) noexcept {
    const auto& peer = connection->conn;
    bool paused = false;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        const auto it = state->connections.find(peer->fd);
        if (it != state->connections.end() && it->second == connection &&
            !connection->read_paused) {
            connection->read_paused = true;
            paused = true;
        }
    }
    if (paused && !refresh_connection_events(state, peer)) {
        peer->closing.store(true, std::memory_order_release);
    }
}

bool pause_connection_reads_if_needed(const std::shared_ptr<ServerState>& state,
                                      const std::shared_ptr<ReactorConnection>& connection) noexcept {
    const auto& peer = connection->conn;
    if (!above_pause_watermark(state, peer)) {
        return false;
    }

    bool should_stop_reading = false;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        const auto it = state->connections.find(peer->fd);
        if (it != state->connections.end() && it->second == connection &&
            above_pause_watermark(state, peer)) {
            should_stop_reading = true;
        }
    }
    if (should_stop_reading) {
        pause_connection_reads(state, connection);
    }
    return should_stop_reading;
}

} // namespace easy_uds::detail
