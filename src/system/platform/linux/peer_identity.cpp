#include "../peer_identity.hpp"

#include <sys/socket.h>
#include <sys/types.h>

namespace easy_uds::detail::peer_identity {

Identity capture(int fd) noexcept {
    Identity identity;
#if defined(SO_PEERCRED)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0) {
        identity.pid = static_cast<std::int64_t>(credentials.pid);
        identity.uid = static_cast<std::uint64_t>(credentials.uid);
        identity.gid = static_cast<std::uint64_t>(credentials.gid);
        identity.present = true;
    }
#else
    (void)fd;
#endif
    return identity;
}

} // namespace easy_uds::detail::peer_identity
