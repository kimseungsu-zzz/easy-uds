#pragma once

// Concrete internal descriptor ownership and socket setup seam.  The current
// implementation is Linux/POSIX, but the public OwnedFd/BorrowedFd API is not
// involved here.  Functions report native errno (0 on success) so callers can
// retain the existing easy_uds::Error mapping at the system boundary.

#include <utility>
#include "native_socket.hpp"

namespace easy_uds::detail {

using NativeSocket = platform_types::NativeSocket;

namespace socket_lifecycle {

using NativeSocket = platform_types::NativeSocket;

enum class SetupFailure {
    none,
    close_on_exec_getfd,
    close_on_exec_setfd,
    nonblocking_getfl,
    nonblocking_setfl,
    no_sigpipe,
};

struct SetupResult {
    int native_error = 0;
    SetupFailure failure = SetupFailure::none;

    [[nodiscard]] bool ok() const noexcept { return native_error == 0; }
};

void close(NativeSocket fd) noexcept;
void shutdown(NativeSocket fd) noexcept;

// Return a native errno plus the failed setup stage; no public Error is built
// at this platform boundary.
SetupResult set_close_on_exec(NativeSocket fd) noexcept;
SetupResult set_nonblocking(NativeSocket fd) noexcept;
SetupResult configure_no_sigpipe(NativeSocket fd) noexcept;

} // namespace socket_lifecycle

class FileDescriptor {
  public:
    explicit FileDescriptor(NativeSocket fd = platform_types::invalid_socket) noexcept : fd_(fd) {}
    ~FileDescriptor() { reset(); }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    [[nodiscard]] NativeSocket get() const noexcept { return fd_; }
    [[nodiscard]] NativeSocket release() noexcept {
        return std::exchange(fd_, platform_types::invalid_socket);
    }

    void reset(NativeSocket fd = platform_types::invalid_socket) noexcept {
        if (platform_types::valid(fd_)) {
            socket_lifecycle::close(fd_);
        }
        fd_ = fd;
    }

  private:
    NativeSocket fd_;
};

} // namespace easy_uds::detail
