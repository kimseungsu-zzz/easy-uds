#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>

#include <utility>

namespace easy_uds::detail::platform_windows {

class HandleOwner {
  public:
    HandleOwner() noexcept = default;
    ~HandleOwner() { reset(); }
    HandleOwner(const HandleOwner&) = delete;
    HandleOwner& operator=(const HandleOwner&) = delete;
    HandleOwner(HandleOwner&& other) noexcept : value_(other.release()) {}
    HandleOwner& operator=(HandleOwner&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = other.release();
        }
        return *this;
    }

    static HandleOwner adopt(HANDLE value) noexcept {
        HandleOwner owner;
        owner.value_ = value == INVALID_HANDLE_VALUE ? nullptr : value;
        return owner;
    }
    [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }
    void reset() noexcept {
        if (value_ != nullptr) {
            ::CloseHandle(value_);
            value_ = nullptr;
        }
    }

  private:
    HANDLE value_ = nullptr;
};

} // namespace easy_uds::detail::platform_windows
#endif
