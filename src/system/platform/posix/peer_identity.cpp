#include "../peer_identity.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/un.h>
#endif

namespace easy_uds::detail::peer_identity {

Identity capture(platform_types::NativeSocket fd) noexcept {
    Identity identity;
#if defined(__linux__) && defined(SO_PEERCRED)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_PEERCRED, &credentials,
                     &length) == 0) {
        identity.pid = static_cast<std::int64_t>(credentials.pid);
        identity.uid = static_cast<std::uint64_t>(credentials.uid);
        identity.gid = static_cast<std::uint64_t>(credentials.gid);
        identity.present = true;
    }
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    if (::getpeereid(static_cast<int>(fd), &uid, &gid) == 0) {
        identity.uid = static_cast<std::uint64_t>(uid);
        identity.gid = static_cast<std::uint64_t>(gid);
        identity.present = true;
    }
#elif defined(__NetBSD__) || defined(__OpenBSD__)
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    if (::getpeereid(static_cast<int>(fd), &uid, &gid) == 0) {
        identity.uid = static_cast<std::uint64_t>(uid);
        identity.gid = static_cast<std::uint64_t>(gid);
        identity.present = true;
    }
#else
    (void)fd;
#endif
    return identity;
}

} // namespace easy_uds::detail::peer_identity
