#pragma once

// Internal RAII ownership for descriptors received by the server.  This is
// deliberately separate from the public POSIX OwnedFd wrapper: system jobs
// own this value, while handlers receive only a borrowed public view.

#include "socket_lifecycle.hpp"

#include <utility>

namespace easy_uds::detail::platform {

class DescriptorOwner {
  public:
    DescriptorOwner() noexcept = default;
    ~DescriptorOwner() { reset(); }

    DescriptorOwner(const DescriptorOwner&) = delete;
    DescriptorOwner& operator=(const DescriptorOwner&) = delete;

    DescriptorOwner(DescriptorOwner&& other) noexcept
        : fd_(other.release()) {}

    DescriptorOwner& operator=(DescriptorOwner&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] static DescriptorOwner adopt(int fd) noexcept {
        DescriptorOwner owner;
        owner.fd_ = fd >= 0 ? fd : -1;
        return owner;
    }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] int native_fd() const noexcept { return fd_; }

    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    void reset() noexcept {
        if (fd_ >= 0) {
            socket_lifecycle::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_ = -1;
};

using descriptor_owner = DescriptorOwner;

} // namespace easy_uds::detail::platform
