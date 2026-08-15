#pragma once

#include "../native_socket.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <cstdint>
#include <string_view>

namespace easy_uds::detail::platform_windows {

[[nodiscard]] bool ensure_winsock() noexcept;
[[nodiscard]] SOCKET to_socket(platform_types::NativeSocket socket) noexcept;
[[nodiscard]] platform_types::NativeSocket from_socket(SOCKET socket) noexcept;
void set_errno_from_wsa(int error) noexcept;
[[nodiscard]] int errno_from_wsa(int error) noexcept;
[[nodiscard]] int last_wsa_error() noexcept;
void remember_bound_path(std::string_view path);
void forget_bound_path(std::string_view path) noexcept;
[[nodiscard]] bool is_bound_path(std::string_view path) noexcept;

} // namespace easy_uds::detail::platform_windows
#endif
