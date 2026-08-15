#pragma once

// Raw byte-I/O and connect-completion capability.  These functions deliberately
// return syscall results and preserve errno; retry, timeout, peer-closed, and
// public Error semantics remain in system/transport.

#include <cstddef>
#include <sys/types.h>
#include <sys/uio.h>

namespace easy_uds::detail::socket_io {

ssize_t receive(int fd, void* data, std::size_t size) noexcept;
ssize_t send(int fd, const void* data, std::size_t size) noexcept;
ssize_t send_iovecs(int fd, iovec* parts, std::size_t part_count) noexcept;
ssize_t send_iovecs_nonblocking(int fd, iovec* parts,
                                std::size_t part_count) noexcept;

// Return 0 on getsockopt success, -1 on failure.  On success, socket_error is
// the native SO_ERROR value and is interpreted by the connection setup layer.
int query_socket_error(int fd, int& socket_error) noexcept;

} // namespace easy_uds::detail::socket_io
