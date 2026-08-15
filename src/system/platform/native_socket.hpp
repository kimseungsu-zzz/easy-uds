#pragma once

// Internal native socket value used by the concrete system/platform seam.
// This is intentionally not a public NativeHandle abstraction: it is only a
// lossless integer carrier for POSIX descriptors and Windows SOCKET values.

#include <cstdint>

namespace easy_uds::detail::platform_types {

using NativeSocket = std::intptr_t;
inline constexpr NativeSocket invalid_socket = static_cast<NativeSocket>(-1);

[[nodiscard]] constexpr bool valid(NativeSocket socket) noexcept {
    return socket != invalid_socket;
}

} // namespace easy_uds::detail::platform_types
