#pragma once

#include "../platform/descriptor_owner.hpp"
#include "../platform/peer_identity.hpp"

namespace easy_uds::detail {

// A job-local snapshot and the one internal owner associated with one
// request.  No public POSIX wrapper is stored here.  The object itself is
// also the private bridge retained by RequestContext during one callback.
struct RequestCapabilityStorage {
    platform::descriptor_owner received_fd;
    peer_identity::Identity peer;
};

using RequestCapabilityBridge = RequestCapabilityStorage;

} // namespace easy_uds::detail
