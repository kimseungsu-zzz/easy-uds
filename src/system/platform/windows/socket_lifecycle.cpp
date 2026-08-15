#include "../socket_lifecycle.hpp"
#include "socket_common.hpp"

#if defined(_WIN32)
#include <cerrno>
#include <windows.h>

namespace easy_uds::detail::socket_lifecycle {

void close(NativeSocket fd) noexcept {
    if (!platform_types::valid(fd)) {
        return;
    }
    if (::closesocket(platform_windows::to_socket(fd)) == SOCKET_ERROR) {
        // The server pathname lock uses a Windows file handle in the same
        // internal integer carrier.  It is not a socket, so close it when the
        // Winsock close reports WSAENOTSOCK.
        if (WSAGetLastError() == WSAENOTSOCK) {
            (void)::CloseHandle(reinterpret_cast<HANDLE>(fd));
        }
    }
}

void shutdown(NativeSocket fd) noexcept {
    if (platform_types::valid(fd)) {
        (void)::shutdown(platform_windows::to_socket(fd), SD_BOTH);
    }
}

SetupResult set_close_on_exec(NativeSocket fd) noexcept {
    (void)fd;
    return {};
}

SetupResult set_nonblocking(NativeSocket fd) noexcept {
    u_long enabled = 1;
    if (::ioctlsocket(platform_windows::to_socket(fd), FIONBIO, &enabled) != 0) {
        (void)platform_windows::last_wsa_error();
        return {errno, SetupFailure::nonblocking_setfl};
    }
    return {};
}

SetupResult configure_no_sigpipe(NativeSocket fd) noexcept {
    (void)fd;
    return {};
}

} // namespace easy_uds::detail::socket_lifecycle
#endif
