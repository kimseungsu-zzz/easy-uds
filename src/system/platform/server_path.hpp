#pragma once

// Concrete Linux pathname/instance lifecycle seam. It reports identity,
// ownership, lock state, and native errors; Server runtime code retains the
// stale/busy policy and public Error mapping.

#include "socket_lifecycle.hpp"

#include <string>
#include <string_view>

#include <sys/types.h>

namespace easy_uds::detail::server_path {

enum class EntryKind {
    missing,
    socket,
    regular,
    other,
};

struct Identity {
    EntryKind kind = EntryKind::missing;
    uid_t owner = 0;
    dev_t device = 0;
    ino_t inode = 0;
    nlink_t links = 0;
};

struct LookupResult {
    bool present = false;
    Identity identity{};
    int native_error = 0;
};

uid_t effective_user() noexcept;
LookupResult inspect(const char* path) noexcept;
bool same_identity(const Identity& left, const Identity& right) noexcept;
bool is_owned_socket(const Identity& identity, uid_t user) noexcept;
bool is_owned_regular_single_link(const Identity& identity, uid_t user) noexcept;

int unlink_socket_path(const char* path) noexcept;
int chmod_socket_path(const char* path, unsigned int permissions) noexcept;

enum class LockStatus {
    acquired,
    busy,
    invalid_entry,
    error,
};

enum class LockFailure {
    none,
    open,
    close_on_exec,
    stat,
    flock,
    chmod,
};

struct LockResult {
    LockStatus status = LockStatus::error;
    LockFailure failure = LockFailure::none;
    socket_lifecycle::SetupFailure setup_failure =
        socket_lifecycle::SetupFailure::none;
    int fd = -1;
    int native_error = 0;
};

std::string lock_path(std::string_view socket_path);
LockResult acquire_lock(std::string_view socket_path);

} // namespace easy_uds::detail::server_path
