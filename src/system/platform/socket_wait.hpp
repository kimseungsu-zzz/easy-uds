#pragma once

// Synchronous one-descriptor wait capability. This is deliberately separate
// from the reactor readiness contract: transport owns deadline calculation and
// retry/error policy, while the platform reports one poll attempt.

#include "native_socket.hpp"

namespace easy_uds::detail::socket_wait {

enum class Interest {
    read,
    write,
};

enum class Status {
    ready,
    timed_out,
    interrupted,
    invalid_descriptor,
    error,
};

struct Result {
    Status status = Status::error;
    int native_error = 0;
};

// Perform one synchronous wait. timeout_ms follows poll semantics: -1 waits
// indefinitely, 0 performs a non-blocking probe, and positive values are
// milliseconds. No public Error is constructed at this boundary.
Result wait_once(platform_types::NativeSocket fd, Interest interest, int timeout_ms) noexcept;

} // namespace easy_uds::detail::socket_wait
