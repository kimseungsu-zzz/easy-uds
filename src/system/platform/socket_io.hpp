#pragma once

// Raw byte-I/O and connect-completion capability.  These functions deliberately
// return syscall results and preserve errno; retry, timeout, peer-closed, and
// public Error semantics remain in system/transport.

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
using ssize_t = std::ptrdiff_t;
struct iovec {
    void* iov_base;
    std::size_t iov_len;
};
#else
#include <sys/types.h>
#include <sys/uio.h>
#endif

#include "native_socket.hpp"

namespace easy_uds::detail::socket_io {

ssize_t receive(platform_types::NativeSocket fd, void* data, std::size_t size) noexcept;
ssize_t send(platform_types::NativeSocket fd, const void* data, std::size_t size) noexcept;
ssize_t send_iovecs(platform_types::NativeSocket fd, iovec* parts, std::size_t part_count) noexcept;
ssize_t send_iovecs_nonblocking(platform_types::NativeSocket fd, iovec* parts,
                                std::size_t part_count) noexcept;

// Return 0 on getsockopt success, -1 on failure.  On success, socket_error is
// the native SO_ERROR value and is interpreted by the connection setup layer.
int query_socket_error(platform_types::NativeSocket fd, int& socket_error) noexcept;

} // namespace easy_uds::detail::socket_io
