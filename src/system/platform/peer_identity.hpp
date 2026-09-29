#pragma once

// Current engine-to-platform seam for connected-peer identity.  The value is
// intentionally internal and does not freeze a cross-platform backend model;
// the Linux implementation currently fills it from the kernel credential
// socket option.

#include <cstdint>
#include <limits>
#include <string>
#include <memory>

#include "native_socket.hpp"

namespace easy_uds::detail::peer_identity {

struct Identity {
    std::int64_t pid = -1;
    std::uint64_t uid = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t gid = std::numeric_limits<std::uint64_t>::max();
    std::string sid;
    std::shared_ptr<void> process_handle;
    bool present = false;
};

Identity capture(platform_types::NativeSocket fd) noexcept;

// Query the connected peer PID where the platform provides that capability.
// Returns false when the query is unavailable or fails.
bool query_peer_process_id(platform_types::NativeSocket fd,
                          std::uint32_t& pid) noexcept;

} // namespace easy_uds::detail::peer_identity
