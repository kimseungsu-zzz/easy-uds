#pragma once

// Concrete Linux pathname/instance lifecycle seam. It reports identity,
// ownership, lock state, and native errors; Server runtime code retains the
// stale/busy policy and public Error mapping.

#include "socket_lifecycle.hpp"

#include <string>
#include <string_view>

#if defined(_WIN32)
#include <cstdint>
using easy_uds_server_path_user_t = std::uint64_t;
using easy_uds_server_path_device_t = std::uint64_t;
using easy_uds_server_path_inode_t = std::uint64_t;
using easy_uds_server_path_links_t = std::uint64_t;
#else
#include <sys/types.h>
using easy_uds_server_path_user_t = uid_t;
using easy_uds_server_path_device_t = dev_t;
using easy_uds_server_path_inode_t = ino_t;
using easy_uds_server_path_links_t = nlink_t;
#endif

namespace easy_uds::detail::server_path {

enum class EntryKind {
    missing,
    socket,
    regular,
    other,
};

struct Identity {
    EntryKind kind = EntryKind::missing;
    easy_uds_server_path_user_t owner = 0;
    easy_uds_server_path_device_t device = 0;
    easy_uds_server_path_inode_t inode = 0;
    easy_uds_server_path_links_t links = 0;
};

struct LookupResult {
    bool present = false;
    Identity identity{};
    int native_error = 0;
};

easy_uds_server_path_user_t effective_user() noexcept;
LookupResult inspect(const char* path) noexcept;
bool same_identity(const Identity& left, const Identity& right) noexcept;
bool is_owned_socket(const Identity& identity,
                     easy_uds_server_path_user_t user) noexcept;
bool is_owned_regular_single_link(const Identity& identity,
                                  easy_uds_server_path_user_t user) noexcept;

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
    platform_types::NativeSocket fd = platform_types::invalid_socket;
    int native_error = 0;
};

std::string lock_path(std::string_view socket_path);
LockResult acquire_lock(std::string_view socket_path);

} // namespace easy_uds::detail::server_path
