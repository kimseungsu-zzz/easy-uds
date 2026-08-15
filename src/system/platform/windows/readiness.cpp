#include "../readiness.hpp"
#include "../socket_lifecycle.hpp"
#include "socket_common.hpp"

#if defined(_WIN32)
#include <algorithm>
#include <array>
#include <cerrno>
#include <mutex>
#include <unordered_map>

namespace easy_uds::detail::readiness {
namespace {

struct Registration {
    std::uint32_t mask = 0;
    std::uint64_t token = 0;
};

struct WakeAddress {
    sockaddr_in address{};
};

std::mutex registry_mutex;
std::unordered_map<platform_types::NativeSocket,
                   std::unordered_map<platform_types::NativeSocket, Registration>>
    pollers;
std::unordered_map<platform_types::NativeSocket, WakeAddress> wakeups;

short native_events(std::uint32_t mask) noexcept {
    short events = 0;
    if ((mask & readable) != 0) {
        events |= POLLRDNORM;
    }
    if ((mask & writable) != 0) {
        events |= POLLWRNORM;
    }
    return events;
}

} // namespace

platform_types::NativeSocket create_poller() noexcept {
    if (!platform_windows::ensure_winsock()) {
        return platform_types::invalid_socket;
    }
    const SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw == INVALID_SOCKET) {
        platform_windows::last_wsa_error();
        return platform_types::invalid_socket;
    }
    const auto fd = platform_windows::from_socket(raw);
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        pollers.emplace(fd, std::unordered_map<platform_types::NativeSocket, Registration>{});
    }
    return fd;
}

platform_types::NativeSocket create_wakeup() noexcept {
    if (!platform_windows::ensure_winsock()) {
        return platform_types::invalid_socket;
    }
    const SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw == INVALID_SOCKET) {
        platform_windows::last_wsa_error();
        return platform_types::invalid_socket;
    }
    const auto fd = platform_windows::from_socket(raw);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(raw, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        platform_windows::last_wsa_error();
        ::closesocket(raw);
        return platform_types::invalid_socket;
    }
    int length = sizeof(address);
    if (::getsockname(raw, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        platform_windows::last_wsa_error();
        ::closesocket(raw);
        return platform_types::invalid_socket;
    }
    const auto setup = socket_lifecycle::set_nonblocking(fd);
    if (!setup.ok()) {
        ::closesocket(raw);
        errno = setup.native_error == 0 ? EIO : setup.native_error;
        return platform_types::invalid_socket;
    }
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        wakeups.emplace(fd, WakeAddress{address});
    }
    return fd;
}

int control(platform_types::NativeSocket poller_fd, Control operation,
            platform_types::NativeSocket fd, std::uint32_t mask,
            std::uint64_t token) noexcept {
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto poller = pollers.find(poller_fd);
    if (poller == pollers.end()) {
        errno = EBADF;
        return -1;
    }
    auto& registrations = poller->second;
    switch (operation) {
    case Control::add:
        if (registrations.find(fd) != registrations.end()) {
            errno = EEXIST;
            return -1;
        }
        registrations.emplace(fd, Registration{mask, token});
        return 0;
    case Control::modify:
        if (const auto it = registrations.find(fd); it != registrations.end()) {
            it->second = Registration{mask, token};
            return 0;
        }
        errno = ENOENT;
        return -1;
    case Control::remove:
        if (registrations.erase(fd) == 0) {
            errno = ENOENT;
            return -1;
        }
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int wait(platform_types::NativeSocket poller_fd, Event* events,
          std::size_t capacity, int timeout_ms) noexcept {
    if (capacity == 0) {
        return 0;
    }
    std::array<WSAPOLLFD, max_events> native{};
    std::array<std::uint64_t, max_events> tokens{};
    const std::size_t limit = std::min(capacity, max_events);
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        const auto poller = pollers.find(poller_fd);
        if (poller == pollers.end()) {
            errno = EBADF;
            return -1;
        }
        for (const auto& [fd, registration] : poller->second) {
            if (count == limit) {
                break;
            }
            native[count].fd = platform_windows::to_socket(fd);
            native[count].events = native_events(registration.mask);
            tokens[count] = registration.token;
            ++count;
        }
    }
    const int result = ::WSAPoll(native.data(), static_cast<ULONG>(count),
                                 timeout_ms < 0 ? -1 : timeout_ms);
    if (result == SOCKET_ERROR) {
        platform_windows::last_wsa_error();
        return -1;
    }
    int output = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const short revents = native[index].revents;
        if (revents == 0) {
            continue;
        }
        std::uint32_t mask = 0;
        if ((revents & POLLRDNORM) != 0) {
            mask |= readable;
        }
        if ((revents & POLLWRNORM) != 0) {
            mask |= writable;
        }
        if ((revents & POLLERR) != 0 || (revents & POLLNVAL) != 0) {
            mask |= error;
        }
        if ((revents & POLLHUP) != 0) {
            mask |= hangup | peer_hangup;
        }
        const auto fd = platform_windows::from_socket(native[index].fd);
        events[output++] = Event{fd, tokens[index], mask};
    }
    return output;
}

void signal(platform_types::NativeSocket wake_fd) noexcept {
    WakeAddress destination;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        const auto it = wakeups.find(wake_fd);
        if (it == wakeups.end()) {
            return;
        }
        destination = it->second;
    }
    const char byte = 1;
    const int result = ::sendto(platform_windows::to_socket(wake_fd), &byte, 1, 0,
                                reinterpret_cast<const sockaddr*>(&destination.address),
                                sizeof(destination.address));
    if (result == SOCKET_ERROR) {
        const int error = platform_windows::last_wsa_error();
        if (error != WSAEWOULDBLOCK) {
            return;
        }
    }
}

void consume(platform_types::NativeSocket wake_fd) noexcept {
    char buffer[64];
    while (true) {
        const int result = ::recv(platform_windows::to_socket(wake_fd), buffer,
                                  sizeof(buffer), 0);
        if (result >= 0) {
            continue;
        }
        const int error = platform_windows::last_wsa_error();
        if (error == WSAEWOULDBLOCK) {
            return;
        }
        return;
    }
}

void close(platform_types::NativeSocket fd) noexcept {
    if (!platform_types::valid(fd)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        pollers.erase(fd);
        wakeups.erase(fd);
    }
    (void)::closesocket(platform_windows::to_socket(fd));
}

} // namespace easy_uds::detail::readiness
#endif
