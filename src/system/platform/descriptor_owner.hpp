#pragma once

// Internal RAII ownership for descriptors received by the server.  This is
// deliberately separate from the public POSIX OwnedFd wrapper: system jobs
// own this value, while handlers receive only a borrowed public view.

#include "socket_lifecycle.hpp"

#include <utility>

namespace easy_uds::detail {

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

    [[nodiscard]] static DescriptorOwner adopt(platform_types::NativeSocket fd) noexcept {
        DescriptorOwner owner;
        owner.fd_ = platform_types::valid(fd) ? fd : platform_types::invalid_socket;
        return owner;
    }

    [[nodiscard]] bool valid() const noexcept { return platform_types::valid(fd_); }
    [[nodiscard]] platform_types::NativeSocket native_fd() const noexcept { return fd_; }

    [[nodiscard]] platform_types::NativeSocket release() noexcept {
        return std::exchange(fd_, platform_types::invalid_socket);
    }

    void reset() noexcept {
        if (platform_types::valid(fd_)) {
            socket_lifecycle::close(fd_);
            fd_ = platform_types::invalid_socket;
        }
    }

  private:
    platform_types::NativeSocket fd_ = platform_types::invalid_socket;
};

using descriptor_owner = DescriptorOwner;

} // namespace easy_uds::detail
