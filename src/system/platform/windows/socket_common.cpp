#include "socket_common.hpp"

#if defined(_WIN32)
#include <algorithm>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_set>

namespace easy_uds::detail::platform_windows {
namespace {

std::once_flag winsock_once;
bool winsock_ready = false;
std::mutex bound_paths_mutex;
std::unordered_set<std::string> bound_paths;

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

int errno_from_wsa(int error) noexcept {
    int mapped = EIO;
    switch (error) {
    // Keep this table explicit rather than relying on Winsock and CRT
    // numeric values happening to match.  The transport uses these errno
    // values for retry/control flow and for the public Error::system_code().
    case WSAEFAULT:
        mapped = EFAULT;
        break;
    case WSAEINTR:
        mapped = EINTR;
        break;
    case WSAEWOULDBLOCK:
        mapped = EAGAIN;
        break;
    case WSAEINPROGRESS:
        mapped = EINPROGRESS;
        break;
    case WSAEALREADY:
        mapped = EALREADY;
        break;
    case WSAEDESTADDRREQ:
        mapped = EDESTADDRREQ;
        break;
    case WSAEMSGSIZE:
        mapped = EMSGSIZE;
        break;
    case WSAEPROTOTYPE:
        mapped = EPROTOTYPE;
        break;
    case WSAENOPROTOOPT:
        mapped = ENOPROTOOPT;
        break;
    case WSAEPROTONOSUPPORT:
        mapped = EPROTONOSUPPORT;
        break;
    case WSAETIMEDOUT:
        mapped = ETIMEDOUT;
        break;
    case WSAECONNRESET:
        mapped = ECONNRESET;
        break;
    case WSAECONNABORTED:
        mapped = ECONNABORTED;
        break;
    case WSAECONNREFUSED:
        mapped = ECONNREFUSED;
        break;
    case WSAEISCONN:
        mapped = EISCONN;
        break;
    case WSAENOTCONN:
        mapped = ENOTCONN;
        break;
    case WSAESHUTDOWN:
        mapped = ESHUTDOWN;
        break;
    case WSAENOTSOCK:
        mapped = EBADF;
        break;
    case WSAEAFNOSUPPORT:
        mapped = EAFNOSUPPORT;
        break;
    case WSAEADDRINUSE:
        mapped = EADDRINUSE;
        break;
    case WSAEADDRNOTAVAIL:
        mapped = EADDRNOTAVAIL;
        break;
    case WSAEACCES:
        mapped = EACCES;
        break;
    case WSAEINVAL:
        mapped = EINVAL;
        break;
    case WSAEBADF:
        mapped = EBADF;
        break;
    case WSAEOPNOTSUPP:
        mapped = EOPNOTSUPP;
        break;
    case WSAEMFILE:
        mapped = EMFILE;
        break;
    case WSAENOBUFS:
        mapped = ENOBUFS;
        break;
    case WSAENETDOWN:
        mapped = ENETDOWN;
        break;
    case WSAENETRESET:
        mapped = ENETRESET;
        break;
    case WSAENETUNREACH:
        mapped = ENETUNREACH;
        break;
    case WSAEHOSTDOWN:
        mapped = EHOSTDOWN;
        break;
    case WSAEHOSTUNREACH:
        mapped = EHOSTUNREACH;
        break;
    default:
        break;
    }
    errno = mapped;
    return mapped;
}

void set_errno_from_wsa(int error) noexcept {
    (void)errno_from_wsa(error);
}

int last_wsa_error() noexcept {
    const int error = WSAGetLastError();
    set_errno_from_wsa(error);
    return error;
}

void remember_bound_path(std::string_view path) {
    std::lock_guard<std::mutex> lock(bound_paths_mutex);
    bound_paths.emplace(path);
}

void forget_bound_path(std::string_view path) noexcept {
    std::lock_guard<std::mutex> lock(bound_paths_mutex);
    bound_paths.erase(std::string(path));
}

bool is_bound_path(std::string_view path) noexcept {
    std::lock_guard<std::mutex> lock(bound_paths_mutex);
    return bound_paths.find(std::string(path)) != bound_paths.end();
}

} // namespace easy_uds::detail::platform_windows
#endif
