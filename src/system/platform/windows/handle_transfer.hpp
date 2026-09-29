#pragma once

#if defined(_WIN32)
#include <cstdint>
#include <winsock2.h>
#include <windows.h>

namespace easy_uds::detail::platform_windows {

HANDLE duplicate_peer_handle(HANDLE peer_process_handle,
                             std::uint64_t peer_handle_value) noexcept;

} // namespace easy_uds::detail::platform_windows
#endif
