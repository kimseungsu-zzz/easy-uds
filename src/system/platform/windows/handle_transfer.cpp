#include "handle_transfer.hpp"

#if defined(_WIN32)
#include <limits>

namespace easy_uds::detail::platform_windows {

HANDLE duplicate_peer_handle(HANDLE peer_process_handle,
                             std::uint64_t peer_handle_value) noexcept {
    if (peer_process_handle == nullptr || peer_handle_value == 0 ||
        peer_handle_value >=
            std::numeric_limits<std::uintptr_t>::max() - 15U) {
        return nullptr;
    }
    HANDLE duplicated = nullptr;
    const BOOL result = ::DuplicateHandle(
        peer_process_handle,
        reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(peer_handle_value)),
        ::GetCurrentProcess(), &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS);
    return result ? duplicated : nullptr;
}

} // namespace easy_uds::detail::platform_windows
#endif
