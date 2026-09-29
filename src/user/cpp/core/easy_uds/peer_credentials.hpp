#pragma once

#include <sys/types.h>

namespace easy_uds {

// POSIX peer identity snapshot. The process id is unavailable on some POSIX
// systems; callers should check `present` and treat a negative pid as absent.
// This header is intentionally separate from the platform-neutral Request.
struct PeerCredentials {
    pid_t pid = -1;
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    bool present = false;
};

} // namespace easy_uds
