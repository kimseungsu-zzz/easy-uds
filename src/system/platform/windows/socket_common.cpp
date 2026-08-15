#include "socket_common.hpp"

#if defined(_WIN32)
#include <cerrno>
#include <mutex>

namespace easy_uds::detail::platform_windows {
namespace {

std::once_flag winsock_once;
bool winsock_ready = false;

} // namespace

bool ensure_winsock() noexcept {
    std::call_once(winsock_once, [] {
        WSADATA data{};
        winsock_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!winsock_ready) {
            errno = EIO;
        }
    });
    return winsock_ready;
}

SOCKET to_socket(platform_types::NativeSocket socket) noexcept {
    return static_cast<SOCKET>(socket);
}

platform_types::NativeSocket from_socket(SOCKET socket) noexcept {
    return static_cast<platform_types::NativeSocket>(socket);
}

void set_errno_from_wsa(int error) noexcept {
    switch (error) {
    case WSAEINTR:
        errno = EINTR;
        break;
    case WSAEWOULDBLOCK:
        errno = EAGAIN;
        break;
    case WSAETIMEDOUT:
        errno = ETIMEDOUT;
        break;
    case WSAECONNRESET:
    case WSAECONNABORTED:
        errno = ECONNRESET;
        break;
    case WSAEPIPE:
        errno = EPIPE;
        break;
    case WSAEADDRINUSE:
        errno = EADDRINUSE;
        break;
    case WSAEACCES:
        errno = EACCES;
        break;
    case WSAEINVAL:
        errno = EINVAL;
        break;
    case WSAEBADF:
        errno = EBADF;
        break;
    default:
        errno = EIO;
        break;
    }
}

int last_wsa_error() noexcept {
    const int error = WSAGetLastError();
    set_errno_from_wsa(error);
    return error;
}

} // namespace easy_uds::detail::platform_windows
#endif
