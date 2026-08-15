#pragma once

#include <string_view>
#include <system_error>
#include <type_traits>

namespace easy_uds {

// Stable, transport-independent failure classes. Detailed operating-system
// information remains available separately through Error::system_code().
enum class ErrorCode {
    system = 1,
    timeout,
    closed,
    protocol,
    busy,
    too_large,
    invalid_request,
    unavailable,
    cancelled,
};

[[nodiscard]] const std::error_category& error_category() noexcept;
[[nodiscard]] std::error_code make_error_code(ErrorCode code) noexcept;

// One exception type with two levels of detail:
//   code()/kind()  - stable easy-uds meaning
//   system_code()  - original errno/error_code, or empty when not applicable
// Error remains a std::system_error, so existing generic system-error handlers
// continue to work.
class Error : public std::system_error {
  public:
    Error(ErrorCode kind, std::string_view operation,
          std::error_code system_code = {});

    [[nodiscard]] ErrorCode kind() const noexcept {
        return static_cast<ErrorCode>(code().value());
    }
    [[nodiscard]] const std::error_code& system_code() const noexcept {
        return system_code_;
    }

  private:
    std::error_code system_code_;
};

namespace detail {

[[nodiscard]] ErrorCode classify_system_error(
    const std::error_code& code) noexcept;

} // namespace detail

} // namespace easy_uds

namespace std {

template <>
struct is_error_code_enum<easy_uds::ErrorCode> : true_type {};

} // namespace std
