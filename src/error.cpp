#include "easy_uds/error.hpp"

#include <string>

namespace easy_uds {
namespace {

class EasyUdsErrorCategory final : public std::error_category {
  public:
    [[nodiscard]] const char* name() const noexcept override { return "easy_uds"; }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<ErrorCode>(value)) {
        case ErrorCode::system:
            return "system error";
        case ErrorCode::timeout:
            return "operation timed out";
        case ErrorCode::closed:
            return "connection closed";
        case ErrorCode::protocol:
            return "protocol error";
        case ErrorCode::busy:
            return "resource busy";
        case ErrorCode::too_large:
            return "message too large";
        case ErrorCode::invalid_request:
            return "invalid request";
        case ErrorCode::unavailable:
            return "service unavailable";
        case ErrorCode::cancelled:
            return "operation cancelled";
        }
        return "unknown easy-uds error";
    }
};

std::string error_context(std::string_view operation,
                          const std::error_code& system_code) {
    std::string context(operation);
    if (system_code) {
        context += " (";
        context += system_code.category().name();
        context += ": ";
        context += system_code.message();
        context += ')';
    }
    return context;
}

} // namespace

const std::error_category& error_category() noexcept {
    static const EasyUdsErrorCategory category;
    return category;
}

std::error_code make_error_code(ErrorCode code) noexcept {
    return {static_cast<int>(code), error_category()};
}

Error::Error(ErrorCode kind, std::string_view operation,
             std::error_code system_code)
    : std::system_error(make_error_code(kind),
                        error_context(operation, system_code)),
      system_code_(system_code) {}

namespace detail {

ErrorCode classify_system_error(const std::error_code& code) noexcept {
    if (code == std::errc::timed_out) {
        return ErrorCode::timeout;
    }
    if (code == std::errc::connection_aborted ||
        code == std::errc::connection_reset || code == std::errc::broken_pipe ||
        code == std::errc::not_connected) {
        return ErrorCode::closed;
    }
    if (code == std::errc::address_in_use ||
        code == std::errc::device_or_resource_busy ||
        code == std::errc::resource_unavailable_try_again) {
        return ErrorCode::busy;
    }
    if (code == std::errc::no_such_file_or_directory ||
        code == std::errc::connection_refused ||
        code == std::errc::network_unreachable ||
        code == std::errc::host_unreachable) {
        return ErrorCode::unavailable;
    }
    if (code == std::errc::message_size) {
        return ErrorCode::too_large;
    }
    if (code == std::errc::operation_canceled) {
        return ErrorCode::cancelled;
    }
    return ErrorCode::system;
}

} // namespace detail

} // namespace easy_uds
