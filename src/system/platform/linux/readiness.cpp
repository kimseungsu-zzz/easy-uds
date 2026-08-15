#include "../readiness.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace easy_uds::detail::readiness {
namespace {

std::uint32_t to_native_mask(std::uint32_t mask) noexcept {
    std::uint32_t native = 0;
    if ((mask & readable) != 0) {
        native |= EPOLLIN;
    }
    if ((mask & writable) != 0) {
        native |= EPOLLOUT;
    }
    if ((mask & error) != 0) {
        native |= EPOLLERR;
    }
    if ((mask & hangup) != 0) {
        native |= EPOLLHUP;
    }
    if ((mask & peer_hangup) != 0) {
        native |= EPOLLRDHUP;
    }
    return native;
}

} // namespace

int create_poller() noexcept {
    return ::epoll_create1(EPOLL_CLOEXEC);
}

int create_wakeup() noexcept {
    return ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

int control(int poller_fd, Control operation, int fd, std::uint32_t mask,
            std::uint64_t token) noexcept {
    if (operation == Control::remove) {
        return ::epoll_ctl(poller_fd, EPOLL_CTL_DEL, fd, nullptr);
    }

    epoll_event event{};
    event.events = to_native_mask(mask);
    event.data.u64 = token;
    const int native_operation = operation == Control::add ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    return ::epoll_ctl(poller_fd, native_operation, fd, &event);
}

int wait(int poller_fd, Event* events, std::size_t capacity, int timeout_ms) noexcept {
    if (events == nullptr || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    std::array<epoll_event, max_events> native_events{};
    const int native_capacity = static_cast<int>(std::min(capacity, max_events));
    const int count = ::epoll_wait(poller_fd, native_events.data(), native_capacity, timeout_ms);
    if (count < 0) {
        return count;
    }
    for (int index = 0; index < count; ++index) {
        events[index].token = native_events[index].data.u64;
        std::uint32_t mask = 0;
        if ((native_events[index].events & EPOLLIN) != 0) {
            mask |= readable;
        }
        if ((native_events[index].events & EPOLLOUT) != 0) {
            mask |= writable;
        }
        if ((native_events[index].events & EPOLLERR) != 0) {
            mask |= error;
        }
        if ((native_events[index].events & EPOLLHUP) != 0) {
            mask |= hangup;
        }
        if ((native_events[index].events & EPOLLRDHUP) != 0) {
            mask |= peer_hangup;
        }
        events[index].mask = mask;
    }
    return count;
}

void signal(int wake_fd) noexcept {
    if (wake_fd < 0) {
        return;
    }
    const std::uint64_t increment = 1;
    while (::write(wake_fd, &increment, sizeof(increment)) < 0 && errno == EINTR) {
    }
}

void consume(int wake_fd) noexcept {
    if (wake_fd < 0) {
        return;
    }
    std::uint64_t counter = 0;
    while (true) {
        const ssize_t result = ::read(wake_fd, &counter, sizeof(counter));
        if (result > 0) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

void close(int fd) noexcept {
    if (fd >= 0) {
        (void)::close(fd);
    }
}

} // namespace easy_uds::detail::readiness
