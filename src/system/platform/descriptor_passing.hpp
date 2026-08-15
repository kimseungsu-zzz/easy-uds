#pragma once

// Current Linux descriptor-passing seam.  The descriptor-bearing operation
// uses the existing POSIX iovec shape deliberately; this is not a generic
// resource-passing or final cross-platform handle contract.

#include <cstddef>

#include <sys/types.h>
#include <sys/uio.h>

namespace easy_uds::detail::descriptor_passing {

enum class ReceiveError {
    none,
    invalid_ancillary,
    close_on_exec_getfd,
    close_on_exec_setfd,
};

struct ReceiveResult {
    ssize_t bytes = -1;
    int received_fd = -1;
    ReceiveError error = ReceiveError::none;
    int native_error = 0;
};

// Sends one iovec batch.  When attach_fd is true, passed_fd is attached to
// this attempt as descriptor ancillary data.  The caller owns retry policy and
// must pass false after the first successful sendmsg so partial sends never
// duplicate the descriptor.
ssize_t send_iovecs(int fd, iovec* parts, std::size_t part_count, int passed_fd,
                    bool attach_fd) noexcept;

// Receives bytes and at most one descriptor. Malformed or truncated ancillary
// data is reported as invalid_ancillary and any descriptor already materialized
// by the kernel is closed before returning. A close-on-exec setup failure
// identifies its fcntl stage and returns native errno; the caller maps that
// result to the public error model. A negative bytes result preserves errno
// from recvmsg itself.
ReceiveResult receive(int fd, void* data, std::size_t size);

} // namespace easy_uds::detail::descriptor_passing
