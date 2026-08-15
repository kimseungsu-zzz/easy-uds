#pragma once

#include "core.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace easy_uds::detail {

inline constexpr std::size_t reactor_read_scratch_size = 64U * 1024U;
inline constexpr std::size_t reactor_read_batch_size = 256U * 1024U;
inline constexpr int max_accept_batch = 64;

inline void clear_reusable_buffer(std::string& buffer) {
    if (buffer.capacity() > reactor_read_scratch_size) {
        std::string{}.swap(buffer);
    } else {
        buffer.clear();
    }
}

inline void mark_io_progress(const std::shared_ptr<Connection>& connection) noexcept {
    connection->last_io_progress.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
}

inline Deadline connection_inactivity_deadline(const std::shared_ptr<Connection>& connection,
                                               std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() == 0) {
        return Deadline::max();
    }
    const auto ticks = connection->last_io_progress.load(std::memory_order_relaxed);
    const Deadline progress{Clock::duration{ticks}};
    const auto max_remaining = Deadline::max() - progress;
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_remaining);
    return timeout >= max_ms ? Deadline::max() - Clock::duration{1} : progress + timeout;
}

inline void wake_reactor(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->wakeup_fd < 0) {
        return;
    }
    bool expected = false;
    if (!state->wake_pending.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_relaxed)) {
        return;
    }
    readiness::signal(state->wakeup_fd);
}

inline std::uint32_t allocate_connection_generation(const std::shared_ptr<ServerState>& state) noexcept {
    std::uint32_t generation = state->next_connection_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0) {
        generation = state->next_connection_generation.fetch_add(1, std::memory_order_relaxed);
    }
    return generation;
}

inline std::uint64_t connection_token(int fd, std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(generation) << 32) | static_cast<std::uint32_t>(fd);
}

} // namespace easy_uds::detail
