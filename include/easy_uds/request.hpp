#pragma once

#include "easy_uds/fd.hpp"

#include <cstdint>
#include <string>

#include <sys/types.h>

namespace easy_uds {

// Peer identity of the connecting client, captured with SO_PEERCRED on Linux.
// `present` is false when the socket cannot provide credentials.
struct PeerCredentials {
    pid_t pid = -1;
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    bool present = false;
};

struct Request {
    std::string route;
    std::string body;
    PeerCredentials peer;
    // Correlation id for the multiplexed protocol. The client assigns it per
    // in-flight request; responses to different requests may arrive in any
    // order. Handlers that mutate shared state must not rely on it being
    // sequential.
    std::uint32_t request_id = 0;
    // Descriptor passed with the request via `request_fd()`, or an empty value
    // when the request carried none. Request owns the received descriptor and
    // closes it automatically. A handler that wants to retain access beyond
    // its return must store `fd.duplicate()`.
    OwnedFd fd;
};

} // namespace easy_uds
