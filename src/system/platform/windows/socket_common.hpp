#pragma once

#include "../native_socket.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <cstdint>

namespace easy_uds::detail::platform_windows {

[[nodiscard]] bool ensure_winsock() noexcept;
[[nodiscard]] SOCKET to_socket(platform_types::NativeSocket socket) noexcept;
[[nodiscard]] platform_types::NativeSocket from_socket(SOCKET socket) noexcept;
void set_errno_from_wsa(int error) noexcept;
[[nodiscard]] int last_wsa_error() noexcept;

} // namespace easy_uds::detail::platform_windows
#endif
