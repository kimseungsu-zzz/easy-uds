#include "../server_path.hpp"

#include <cerrno>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace easy_uds::detail::server_path {
namespace {

EntryKind entry_kind(mode_t mode) noexcept {
    if (S_ISSOCK(mode)) {
        return EntryKind::socket;
    }
    if (S_ISREG(mode)) {
        return EntryKind::regular;
    }
    return EntryKind::other;
}

} // namespace

uid_t effective_user() noexcept {
    return ::geteuid();
}

LookupResult inspect(const char* path) noexcept {
    struct stat info {};
    if (::lstat(path, &info) != 0) {
        return {false, {}, errno};
    }
    return {true,
            {entry_kind(info.st_mode), info.st_uid, info.st_dev, info.st_ino,
             info.st_nlink},
            0};
}

bool same_identity(const Identity& left, const Identity& right) noexcept {
    return left.device == right.device && left.inode == right.inode;
}

bool is_owned_socket(const Identity& identity, uid_t user) noexcept {
    return identity.kind == EntryKind::socket && identity.owner == user;
}

bool is_owned_regular_single_link(const Identity& identity, uid_t user) noexcept {
    return identity.kind == EntryKind::regular && identity.owner == user &&
           identity.links == 1;
}

int unlink_socket_path(const char* path) noexcept {
    return ::unlink(path);
}

int chmod_socket_path(const char* path, unsigned int permissions) noexcept {
    return ::chmod(path, static_cast<mode_t>(permissions));
}

std::string lock_path(std::string_view socket_path) {
    return std::string(socket_path) + ".lock";
}

LockResult acquire_lock(std::string_view socket_path) {
    const std::string path = lock_path(socket_path);
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif

    socket_lifecycle::NativeSocket raw_fd = ::open(path.c_str(), flags, 0600);
    FileDescriptor fd(raw_fd);
    if (fd.get() < 0) {
        return {LockStatus::error, LockFailure::open,
                socket_lifecycle::SetupFailure::none, -1, errno};
    }

    const auto close_on_exec = socket_lifecycle::set_close_on_exec(fd.get());
    if (!close_on_exec.ok()) {
        return {LockStatus::error, LockFailure::close_on_exec,
                close_on_exec.failure, -1, close_on_exec.native_error};
    }

    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        return {LockStatus::error, LockFailure::stat,
                socket_lifecycle::SetupFailure::none, -1, errno};
    }
    const Identity identity{entry_kind(info.st_mode), info.st_uid, info.st_dev,
                            info.st_ino, info.st_nlink};
    if (!is_owned_regular_single_link(identity, effective_user())) {
        return {LockStatus::invalid_entry, LockFailure::stat,
                socket_lifecycle::SetupFailure::none, -1, 0};
    }

    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        if (error == EWOULDBLOCK || error == EAGAIN) {
            return {LockStatus::busy, LockFailure::flock,
                    socket_lifecycle::SetupFailure::none, -1, error};
        }
        return {LockStatus::error, LockFailure::flock,
                socket_lifecycle::SetupFailure::none, -1, error};
    }
    if (::fchmod(fd.get(), 0600) != 0) {
        return {LockStatus::error, LockFailure::chmod,
                socket_lifecycle::SetupFailure::none, -1, errno};
    }

    return {LockStatus::acquired, LockFailure::none,
            socket_lifecycle::SetupFailure::none, fd.release(), 0};
}

} // namespace easy_uds::detail::server_path
