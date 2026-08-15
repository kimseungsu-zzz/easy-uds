#pragma once

// Current engine-to-platform seam for connected-peer identity.  The value is
// intentionally internal and does not freeze a cross-platform backend model;
// the Linux implementation currently fills it from the kernel credential
// socket option.

#include <cstdint>
#include <limits>

namespace easy_uds::detail::peer_identity {

struct Identity {
    std::int64_t pid = -1;
    std::uint64_t uid = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t gid = std::numeric_limits<std::uint64_t>::max();
    bool present = false;
};

Identity capture(int fd) noexcept;

} // namespace easy_uds::detail::peer_identity
