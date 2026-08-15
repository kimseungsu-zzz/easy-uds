#include "../peer_identity.hpp"

#if defined(_WIN32)
namespace easy_uds::detail::peer_identity {

Identity capture(platform_types::NativeSocket fd) noexcept {
    (void)fd;
    // Windows PID/token identity is deliberately not represented as Linux
    // pid/uid/gid credentials in 0.8.  The public POSIX capability is absent.
    return {};
}

} // namespace easy_uds::detail::peer_identity
#endif
