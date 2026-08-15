#include "../server_path.hpp"
#include "socket_common.hpp"

#if defined(_WIN32)
#include <cerrno>
#include <windows.h>

#include <functional>

namespace easy_uds::detail::server_path {
namespace {

void set_errno_from_win32(DWORD error) noexcept {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
        errno = EACCES;
        break;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EBUSY;
        break;
    default:
        errno = EIO;
        break;
    }
}

} // namespace

easy_uds_server_path_user_t effective_user() noexcept {
    // 0 is the documented "identity unavailable" value for this first
    // Windows backend.  PID/SID/token authentication is a separate future
    // capability and is not aliased to Linux uid/gid semantics.
    return 0;
}

LookupResult inspect(const char* path) noexcept {
    const DWORD attributes = ::GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = ::GetLastError();
        if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) &&
            platform_windows::is_bound_path(path)) {
            const auto path_hash = static_cast<std::uint64_t>(
                std::hash<std::string_view>{}(path));
            return {true, {EntryKind::socket, 0, 0, path_hash, 1}, 0};
        }
        set_errno_from_win32(error);
        return {false, {}, static_cast<int>(error)};
    }
    const auto path_hash = static_cast<std::uint64_t>(std::hash<std::string_view>{}(path));
    // Windows does not expose a POSIX socket inode/type through
    // GetFileAttributes. Treat only paths known to this backend as sockets;
    // an unrelated existing file must never become eligible for stale-socket
    // removal merely because it is not a directory.
    const EntryKind kind = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                               ? EntryKind::other
                               : (platform_windows::is_bound_path(path)
                                      ? EntryKind::socket
                                      : EntryKind::regular);
    return {true, {kind, 0, 0, path_hash, 1}, 0};
}

bool same_identity(const Identity& left, const Identity& right) noexcept {
    return left.device == right.device && left.inode == right.inode;
}

bool is_owned_socket(const Identity& identity,
                     easy_uds_server_path_user_t user) noexcept {
    return identity.kind == EntryKind::socket && identity.owner == user;
}

bool is_owned_regular_single_link(const Identity& identity,
                                  easy_uds_server_path_user_t user) noexcept {
    return identity.kind == EntryKind::regular && identity.owner == user &&
           identity.links == 1;
}

int unlink_socket_path(const char* path) noexcept {
    if (::DeleteFileA(path) != 0) {
        platform_windows::forget_bound_path(path);
        return 0;
    }
    if (platform_windows::is_bound_path(path)) {
        platform_windows::forget_bound_path(path);
        return 0;
    }
    set_errno_from_win32(::GetLastError());
    return -1;
}

int chmod_socket_path(const char* path, unsigned int permissions) noexcept {
    (void)path;
    (void)permissions;
    // Windows ACLs are intentionally outside the 0.8 initial endpoint scope.
    return 0;
}

std::string lock_path(std::string_view socket_path) {
    return std::string(socket_path) + ".lock";
}

LockResult acquire_lock(std::string_view socket_path) {
    const std::string path = lock_path(socket_path);
    HANDLE handle = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   0, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                   nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = ::GetLastError();
        set_errno_from_win32(error);
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
            return {LockStatus::busy, LockFailure::flock,
                    socket_lifecycle::SetupFailure::none,
                    platform_types::invalid_socket, static_cast<int>(error)};
        }
        return {LockStatus::error, LockFailure::open,
                socket_lifecycle::SetupFailure::none,
                platform_types::invalid_socket, static_cast<int>(error)};
    }
    return {LockStatus::acquired, LockFailure::none,
            socket_lifecycle::SetupFailure::none,
            reinterpret_cast<platform_types::NativeSocket>(handle), 0};
}

} // namespace easy_uds::detail::server_path
#endif
