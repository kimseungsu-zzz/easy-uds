#pragma once

#include "easy_uds/error.hpp"

#include <cerrno>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace easy_uds {

// A non-owning descriptor view. Destroying or moving this value never closes
// the descriptor. The owner must keep the descriptor open while the view is
// used.
class BorrowedFd {
  public:
    constexpr BorrowedFd() noexcept = default;
    explicit constexpr BorrowedFd(int fd) noexcept : fd_(fd >= 0 ? fd : -1) {}

    [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] constexpr int get() const noexcept { return fd_; }

  private:
    int fd_ = -1;
};

// A unique, owning descriptor. Ownership is transferred by move and the
// descriptor is closed automatically. The wrapper has no allocation and is
// intentionally the size of one int.
class OwnedFd {
  public:
    constexpr OwnedFd() noexcept = default;
    ~OwnedFd() { reset(); }

    OwnedFd(const OwnedFd&) = delete;
    OwnedFd& operator=(const OwnedFd&) = delete;

    OwnedFd(OwnedFd&& other) noexcept : fd_(other.release()) {}
    OwnedFd& operator=(OwnedFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.release();
        }
        return *this;
    }

    // Explicitly adopts ownership of `fd`. Passing -1 creates an empty value.
    [[nodiscard]] static OwnedFd adopt(int fd) noexcept {
        return OwnedFd(fd >= 0 ? fd : -1, AdoptTag{});
    }

    [[nodiscard]] constexpr bool valid() const noexcept { return fd_ >= 0; }
    [[nodiscard]] constexpr int get() const noexcept { return fd_; }
    [[nodiscard]] constexpr BorrowedFd borrow() const noexcept { return BorrowedFd{fd_}; }

    // Creates another descriptor for the same open file description. The
    // returned value owns its descriptor and has close-on-exec set.
    [[nodiscard]] OwnedFd duplicate() const {
        if (!valid()) {
            throw Error(ErrorCode::invalid_request,
                        "cannot duplicate an empty descriptor",
                        {EBADF, std::generic_category()});
        }
        int duplicate_fd;
        do {
            duplicate_fd = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
        } while (duplicate_fd < 0 && errno == EINTR);
        if (duplicate_fd < 0) {
            const std::error_code system_code(errno, std::generic_category());
            throw Error(detail::classify_system_error(system_code),
                        "fcntl(F_DUPFD_CLOEXEC) failed", system_code);
        }
        return adopt(duplicate_fd);
    }

    // Relinquishes ownership without closing. The caller becomes responsible
    // for the returned descriptor.
    [[nodiscard]] int release() noexcept {
        return std::exchange(fd_, -1);
    }

    // Closes the currently owned descriptor, if any.
    void reset() noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
            fd_ = -1;
        }
    }

  private:
    struct AdoptTag {};
    constexpr OwnedFd(int fd, AdoptTag) noexcept : fd_(fd) {}

    int fd_ = -1;
};

[[nodiscard]] constexpr BorrowedFd borrow_fd(int fd) noexcept {
    return BorrowedFd{fd};
}

static_assert(sizeof(BorrowedFd) == sizeof(int), "BorrowedFd must stay allocation-free");
static_assert(sizeof(OwnedFd) == sizeof(int), "OwnedFd must stay allocation-free");

} // namespace easy_uds
