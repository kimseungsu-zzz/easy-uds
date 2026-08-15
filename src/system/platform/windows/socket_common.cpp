#include "socket_common.hpp"

#if defined(_WIN32)
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
    case WSAECONNREFUSED:
        errno = ECONNREFUSED;
        break;
    case WSAENOTCONN:
        errno = ENOTCONN;
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
