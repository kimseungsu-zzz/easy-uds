#pragma once

#include "../platform/descriptor_owner.hpp"
#include "../platform/peer_identity.hpp"
#if defined(_WIN32)
#include "../platform/windows/handle_owner.hpp"
#endif

#include <atomic>
#include <memory>
#include <cstddef>

namespace easy_uds::detail {

// A job-local snapshot and the one internal owner associated with one
// request.  No public POSIX wrapper is stored here.  The object itself is
// also the private bridge retained by RequestContext during one callback.
struct RequestCapabilityStorage {
    descriptor_owner received_fd;
    peer_identity::Identity peer;
    std::shared_ptr<std::atomic<bool>> cancelled;
    std::size_t wire_resource_bytes = 0;
#if defined(_WIN32)
    std::uint64_t peer_handle_value = 0;
    mutable platform_windows::HandleOwner received_handle;
#endif
};

using RequestCapabilityBridge = RequestCapabilityStorage;

} // namespace easy_uds::detail
