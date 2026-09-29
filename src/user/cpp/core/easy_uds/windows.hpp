#pragma once

#if !defined(_WIN32)
#error "easy_uds/windows.hpp is available only on Windows"
#endif

#include "easy_uds/error.hpp"
#include "easy_uds/request_context.hpp"

#include <winsock2.h>
#include <windows.h>

#include <system_error>
#include <utility>

namespace easy_uds::detail {
struct RequestCapabilityStorage;
void* request_capability_windows_handle(
    const RequestCapabilityStorage*) noexcept;
} // namespace easy_uds::detail

namespace easy_uds::windows {

class OwnedHandle;

class BorrowedHandle {
  public:
    constexpr BorrowedHandle() noexcept = default;
    explicit constexpr BorrowedHandle(HANDLE value) noexcept : value_(value) {}
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] constexpr HANDLE get() const noexcept { return value_; }
    [[nodiscard]] OwnedHandle duplicate() const;

  private:
    HANDLE value_ = nullptr;
};

class OwnedHandle {
  public:
    OwnedHandle() noexcept = default;
    ~OwnedHandle() { reset(); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    static OwnedHandle adopt(HANDLE value) noexcept {
        OwnedHandle result;
        result.value_ = value == INVALID_HANDLE_VALUE ? nullptr : value;
        return result;
    }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] BorrowedHandle borrow() const noexcept {
        return BorrowedHandle{value_};
    }
    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }
    void reset() noexcept {
        if (valid()) {
            ::CloseHandle(value_);
            value_ = nullptr;
        }
    }

  private:
    HANDLE value_ = nullptr;
};

inline OwnedHandle BorrowedHandle::duplicate() const {
    if (!valid()) {
        throw Error(ErrorCode::invalid_request,
                    "cannot duplicate an empty Windows handle");
    }
    HANDLE duplicate = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), value_, ::GetCurrentProcess(),
                           &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        const auto error = static_cast<int>(::GetLastError());
        throw Error(ErrorCode::system, "DuplicateHandle failed",
                    {error, std::system_category()});
    }
    return OwnedHandle::adopt(duplicate);
}

class RequestCapabilities {
  public:
    RequestCapabilities() noexcept = default;
    [[nodiscard]] BorrowedHandle received_handle() const noexcept {
        return BorrowedHandle{static_cast<HANDLE>(
            detail::request_capability_windows_handle(bridge_))};
    }

  private:
    friend RequestCapabilities request_capabilities(
        const RequestContext&) noexcept;
    explicit RequestCapabilities(
        const detail::RequestCapabilityStorage* bridge) noexcept
        : bridge_(bridge) {}
    const detail::RequestCapabilityStorage* bridge_ = nullptr;
};

[[nodiscard]] inline RequestCapabilities request_capabilities(
    const RequestContext& context) noexcept {
    return RequestCapabilities{detail::request_capability_bridge(context)};
}

} // namespace easy_uds::windows
