#pragma once

// Platform-neutral readiness contract used by the reactor.  The Linux
// implementation in platform/linux/readiness.cpp translates this small value
// vocabulary to epoll and eventfd without exposing those types to policy or
// state-machine code.

#include <cstddef>
#include <cstdint>

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
    std::uint64_t token = 0;
    std::uint32_t mask = 0;
};

// All functions are concrete build-time backend calls.  A negative return
// preserves errno for the caller's existing Error translation.
int create_poller() noexcept;
int create_wakeup() noexcept;
int control(int poller_fd, Control operation, int fd, std::uint32_t mask,
            std::uint64_t token) noexcept;
int wait(int poller_fd, Event* events, std::size_t capacity,
         int timeout_ms) noexcept;

void signal(int wake_fd) noexcept;
void consume(int wake_fd) noexcept;
void close(int fd) noexcept;

} // namespace easy_uds::detail::readiness
