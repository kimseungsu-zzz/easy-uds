#include "../readiness.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

namespace easy_uds::detail::readiness {
namespace {
std::mutex wakeup_mutex;
std::unordered_map<platform_types::NativeSocket,
                   platform_types::NativeSocket> wakeup_writers;

bool configure_fd(int fd) noexcept {
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    const int status_flags = ::fcntl(fd, F_GETFL);
    return descriptor_flags >= 0 && status_flags >= 0 &&
           ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0 &&
           ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) == 0;
}

void set_change(struct kevent& change, platform_types::NativeSocket fd,
                short filter, std::uint32_t mask, std::uint64_t token) noexcept {
    const bool enabled = filter == EVFILT_READ ? (mask & readable) != 0
                                               : (mask & writable) != 0;
    EV_SET(&change, static_cast<uintptr_t>(fd), filter,
           static_cast<u_short>(EV_ADD | (enabled ? EV_ENABLE : EV_DISABLE)),
           0, 0, reinterpret_cast<void*>(static_cast<uintptr_t>(token)));
}

} // namespace

platform_types::NativeSocket create_poller() noexcept {
    const int fd = ::kqueue();
    if (fd < 0) {
        return platform_types::invalid_socket;
    }
    if (!configure_fd(fd)) {
        const int error = errno;
        (void)::close(fd);
        errno = error;
        return platform_types::invalid_socket;
    }
    return fd;
}

platform_types::NativeSocket create_wakeup() noexcept {
    int descriptors[2] = {-1, -1};
    if (::pipe(descriptors) != 0) {
        return platform_types::invalid_socket;
    }
    if (!configure_fd(descriptors[0]) || !configure_fd(descriptors[1])) {
        const int error = errno;
        (void)::close(descriptors[0]);
        (void)::close(descriptors[1]);
        errno = error;
        return platform_types::invalid_socket;
    }
    try {
        std::lock_guard<std::mutex> lock(wakeup_mutex);
        wakeup_writers.emplace(descriptors[0], descriptors[1]);
    } catch (...) {
        (void)::close(descriptors[0]);
        (void)::close(descriptors[1]);
        errno = ENOMEM;
        return platform_types::invalid_socket;
    }
    return descriptors[0];
}

int control(platform_types::NativeSocket poller_fd, Control operation,
            platform_types::NativeSocket fd, std::uint32_t mask,
            std::uint64_t token) noexcept {
    if (operation == Control::remove) {
        struct kevent changes[2]{};
        EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE,
               0, 0, nullptr);
        EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE,
               0, 0, nullptr);
        for (const auto& change : changes) {
            if (::kevent(static_cast<int>(poller_fd), &change, 1, nullptr, 0,
                         nullptr) != 0 && errno != ENOENT) {
                return -1;
            }
        }
        return 0;
    }

    struct kevent changes[2]{};
    set_change(changes[0], fd, EVFILT_READ, mask, token);
    set_change(changes[1], fd, EVFILT_WRITE, mask, token);
    return ::kevent(static_cast<int>(poller_fd), changes, 2, nullptr, 0,
                    nullptr);
}

int wait(platform_types::NativeSocket poller_fd, Event* events,
         std::size_t capacity, int timeout_ms) noexcept {
    if (events == nullptr || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    std::array<struct kevent, max_events> native_events{};
    const int native_capacity = static_cast<int>(std::min(capacity, max_events));
    struct timespec timeout{};
    struct timespec* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
        timeout_ptr = &timeout;
    }
    const int count = ::kevent(static_cast<int>(poller_fd), nullptr, 0,
                               native_events.data(), native_capacity,
                               timeout_ptr);
    if (count < 0) {
        return count;
    }
    int output_count = 0;
    for (int index = 0; index < count; ++index) {
        const auto fd = static_cast<platform_types::NativeSocket>(
            native_events[index].ident);
        const auto token = static_cast<std::uint64_t>(
            reinterpret_cast<uintptr_t>(native_events[index].udata));
        std::uint32_t mask = 0;
        if (native_events[index].filter == EVFILT_READ) {
            mask |= readable;
        } else if (native_events[index].filter == EVFILT_WRITE) {
            mask |= writable;
        }
        if ((native_events[index].flags & EV_ERROR) != 0) {
            mask |= error;
        }
        if ((native_events[index].flags & EV_EOF) != 0) {
            mask |= hangup | peer_hangup;
        }
        int existing = 0;
        while (existing < output_count && events[existing].fd != fd) {
            ++existing;
        }
        if (existing < output_count) {
            events[existing].mask |= mask;
        } else {
            events[output_count++] = {fd, token, mask};
        }
    }
    return output_count;
}

void signal(platform_types::NativeSocket wake_fd) noexcept {
    std::lock_guard<std::mutex> lock(wakeup_mutex);
    const auto it = wakeup_writers.find(wake_fd);
    if (it == wakeup_writers.end()) {
        return;
    }
    const char byte = 1;
    while (::write(static_cast<int>(it->second), &byte, sizeof(byte)) < 0 &&
           errno == EINTR) {
    }
}

void consume(platform_types::NativeSocket wake_fd) noexcept {
    char bytes[128];
    while (true) {
        const auto result = ::read(static_cast<int>(wake_fd), bytes, sizeof(bytes));
        if (result > 0 || (result < 0 && errno == EINTR)) {
            continue;
        }
        return;
    }
}

void close(platform_types::NativeSocket fd) noexcept {
    if (fd < 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(wakeup_mutex);
    const auto it = wakeup_writers.find(fd);
    if (it != wakeup_writers.end()) {
        (void)::close(static_cast<int>(it->second));
        wakeup_writers.erase(it);
    }
    (void)::close(static_cast<int>(fd));
}

} // namespace easy_uds::detail::readiness
