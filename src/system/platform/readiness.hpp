#pragma once

// Current reactor readiness contract used by the reactor. The Linux
// implementation in platform/linux/readiness.cpp translates this small value
// vocabulary to epoll and eventfd without exposing those types to policy or
// state-machine code. This is not the final cross-platform backend contract.

#include <cstddef>
#include <cstdint>

#include "native_socket.hpp"

namespace easy_uds::detail::readiness {

inline constexpr std::uint32_t readable = 1U << 0;
inline constexpr std::uint32_t writable = 1U << 1;
inline constexpr std::uint32_t error = 1U << 2;
inline constexpr std::uint32_t hangup = 1U << 3;
inline constexpr std::uint32_t peer_hangup = 1U << 4;

inline constexpr std::size_t max_events = 128;

enum class Control {
    add,
    modify,
    remove,
};

struct Event {
    // The concrete native descriptor is carried separately from the token so
    // Windows SOCKET values are not truncated to the token's 32-bit routing
    // portion. The token continues to carry generation/stale-event state.
    platform_types::NativeSocket fd = platform_types::invalid_socket;
    std::uint64_t token = 0;
    std::uint32_t mask = 0;
};

// All functions are concrete build-time backend calls.  A negative return
// preserves errno for the caller's existing Error translation.
platform_types::NativeSocket create_poller() noexcept;
platform_types::NativeSocket create_wakeup() noexcept;
int control(platform_types::NativeSocket poller_fd, Control operation,
            platform_types::NativeSocket fd, std::uint32_t mask,
            std::uint64_t token) noexcept;
int wait(platform_types::NativeSocket poller_fd, Event* events, std::size_t capacity,
         int timeout_ms) noexcept;

void signal(platform_types::NativeSocket wake_fd) noexcept;
void consume(platform_types::NativeSocket wake_fd) noexcept;
void close(platform_types::NativeSocket fd) noexcept;

} // namespace easy_uds::detail::readiness
