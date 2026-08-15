#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace easy_uds {

// Cumulative server event counters are opt-in. Operational gauges remain
// available in every ServerStats snapshot because they reuse accounting that
// the reactor already maintains for correctness and backpressure.
enum class StatsMode {
    disabled = 0,
    basic = 1,
};

struct ServerStatsCounters {
    std::uint64_t accepted_connections = 0;
    std::uint64_t rejected_connections = 0;
    std::uint64_t fixed_requests_dispatched = 0;
    std::uint64_t stream_requests_started = 0;
    std::uint64_t stream_requests_rejected = 0;
    std::uint64_t requests_timed_out_before_execution = 0;
    std::uint64_t serialized_requests_superseded = 0;
    std::uint64_t serialized_requests_rejected_busy = 0;
};

// A best-effort operational snapshot. Fields are individually race-free but
// are sampled from separate subsystems, so the struct is not one atomic point
// in time. See docs/api/stats.md for exact accounting boundaries.
struct ServerStats {
    bool running = false;
    std::size_t active_connections = 0;
    std::size_t active_streams = 0;
    std::size_t inflight_requests = 0;
    std::size_t retained_request_bytes = 0;
    std::size_t queued_output_bytes = 0;
    std::size_t worker_queue_depth = 0;
    std::size_t serialized_queue_depth = 0;
    std::size_t active_serialized_domains = 0;
    std::optional<ServerStatsCounters> counters;
};

struct SessionStatsCounters {
    std::uint64_t requests_started = 0;
    std::uint64_t requests_completed = 0;
    std::uint64_t requests_timed_out = 0;
    std::uint64_t requests_failed = 0;
};

// Fixed-request Session accounting. Dedicated request_stream() connections do
// not use the multiplexed Session state and are intentionally excluded.
struct SessionStats {
    std::size_t inflight_requests = 0;
    std::optional<SessionStatsCounters> counters;
};

} // namespace easy_uds
