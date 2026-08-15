#pragma once

// Current Linux descriptor-passing seam.  The descriptor-bearing operation
// uses the existing POSIX iovec shape deliberately; this is not a generic
// resource-passing or final cross-platform handle contract.

#include <cstddef>

#include <sys/types.h>
#include <sys/uio.h>

namespace easy_uds::detail::descriptor_passing {

// Sends one iovec batch.  When attach_fd is true, passed_fd is attached to
// this attempt as descriptor ancillary data.  The caller owns retry policy and
// must pass false after the first successful sendmsg so partial sends never
// duplicate the descriptor.
ssize_t send_iovecs(int fd, iovec* parts, std::size_t part_count, int passed_fd,
                    bool attach_fd) noexcept;

// Receives bytes and at most one descriptor.  received_fd is set to -1 when
// no descriptor arrived.  Malformed or truncated ancillary data is fatal and
// any descriptor already materialized by the kernel is closed before the
// function throws, preserving the parser's existing desynchronization rule.
ssize_t receive(int fd, void* data, std::size_t size, int& received_fd);

} // namespace easy_uds::detail::descriptor_passing
