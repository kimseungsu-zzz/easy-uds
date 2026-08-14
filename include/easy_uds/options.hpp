#pragma once

#include "easy_uds/stats.hpp"

#include <chrono>
#include <cstddef>

namespace easy_uds {

inline constexpr std::size_t default_max_message_size = 1024U * 1024U;
inline constexpr std::size_t default_stream_chunk_size = 64U * 1024U;
inline constexpr std::size_t default_max_stream_size = 1024U * 1024U * 1024U;
inline constexpr std::size_t default_max_inflight_requests_per_connection = 64;
inline constexpr std::size_t default_max_inflight_request_bytes_per_connection =
    4U * 1024U * 1024U;
inline constexpr std::size_t default_max_output_bytes_per_connection = 4U * 1024U * 1024U;

struct ServerOptions {
    // Fixed worker-pool size used to execute handlers. Must be > 0.
    std::size_t worker_threads = 4;

    // Maximum number of concurrently open client connections.
    std::size_t max_connections = 64;

    // Maximum route+body request bytes and maximum response-body bytes.
    std::size_t max_message_size = default_max_message_size;

    // Buffer and wire-frame size used for streamed request/response bodies.
    std::size_t stream_chunk_size = default_stream_chunk_size;

    // Maximum bytes in each streamed request body and response body. Zero
    // allows an unbounded stream; io_timeout still detects stalled peers.
    std::size_t max_stream_size = default_max_stream_size;

    // Strict aggregate declared route+body budget across all connections. A
    // frame reserves its bytes after header validation and before parser
    // buffers are allocated, so partial, queued, and executing requests are
    // included. Zero disables the aggregate limit and preserves the session
    // continuation fast path.
    std::size_t max_total_inflight_bytes = 0;

    // Aggregate unsent fixed-response wire-byte budget (header + remaining
    // body) across all connections. Zero disables the global limit;
    // per-connection output limits remain.
    std::size_t max_total_output_bytes = 0;

    // Maximum number of queued or executing fixed requests per connection.
    // This is the primary per-peer memory/backpressure guard.
    std::size_t max_inflight_requests_per_connection =
        default_max_inflight_requests_per_connection;

    // Maximum aggregate route+body bytes retained for queued or executing
    // fixed requests on one connection. Must fit at least one max_message.
    std::size_t max_inflight_request_bytes_per_connection =
        default_max_inflight_request_bytes_per_connection;

    // Maximum unsent fixed-response wire bytes for one connection. A slow peer
    // exceeding this limit is closed; the default preserves the 4 MiB cap.
    std::size_t max_output_bytes_per_connection = default_max_output_bytes_per_connection;

    // Maximum simultaneous streams. Zero means automatic: reserve one worker
    // for regular RPC (`worker_threads - 1`, at least 1). Explicit values must
    // be between 1 and worker_threads.
    std::size_t max_concurrent_streams = 0;

    // Maximum idle time between successful socket-I/O progress events.
    // Zero disables the inactivity timeout.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for a request, measured from its first header byte
    // until its response is written. Zero disables it. Handler execution
    // is not forcibly interrupted; if it runs past the deadline, response I/O
    // fails immediately when the handler returns.
    std::chrono::milliseconds request_timeout{30000};

    // Absolute deadline for a streaming exchange after its stream header has
    // arrived. Zero allows a long-lived stream, bounded only by io_timeout.
    std::chrono::milliseconds stream_timeout{0};

    // A worker that just served a fixed request waits directly for one next
    // request during this grace period (avoiding a reactor dispatch hop). It
    // returns the connection to the reactor before executing that request, so
    // later multiplexed requests can still run concurrently. `0` disables the
    // continuation fast path.
    std::chrono::milliseconds session_idle_grace{1};

    // When a socket pathname exists but refuses connections, wait this long
    // before considering it stale. Zero performs no grace wait.
    std::chrono::milliseconds stale_socket_grace_period{250};

    // Backlog passed to listen().
    int listen_backlog = 64;

    // Filesystem mode applied to the Unix socket pathname.
    unsigned int socket_permissions = 0600;

    // Include handler exception messages (and response-rejection reasons) in
    // 500 response bodies so clients can see the root cause. Disable when
    // clients must not learn internal error details.
    bool include_handler_error_messages = true;

    // Optional cumulative event counters. Disabled preserves the default hot
    // path; Server::stats() operational gauges remain available either way.
    StatsMode stats = StatsMode::disabled;
};

struct ClientOptions {
    std::size_t max_message_size = default_max_message_size;
    std::size_t stream_chunk_size = default_stream_chunk_size;
    // Zero allows an unbounded stream.
    std::size_t max_stream_size = default_max_stream_size;

    // Maximum time spent establishing a connection. Zero disables this
    // connect-specific limit; request_timeout may still bound the operation.
    std::chrono::milliseconds connect_timeout{2000};

    // Maximum idle time between successful socket-I/O progress events.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for connect + request write + response read.
    std::chrono::milliseconds request_timeout{30000};

    // Absolute deadline for connect + streamed request + streamed response.
    std::chrono::milliseconds stream_timeout{0};

    // Optional cumulative counters for Sessions created by this Client.
    // In-flight depth remains observable when disabled. One-shot calls keep
    // no persistent accounting state.
    StatsMode stats = StatsMode::disabled;
};

} // namespace easy_uds
