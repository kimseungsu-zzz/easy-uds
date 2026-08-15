#pragma once

// Server-side core types shared by the reactor, the worker pool, and the
// public Server implementation.

#include "easy_uds/server.hpp"
#include "../platform/peer_identity.hpp"
#include "../platform/readiness.hpp"
#include "../transport/io.hpp"

#include <atomic>
#include <array>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace easy_uds::detail {

inline constexpr unsigned char handler_serialized_flag = 0x01U;
inline constexpr unsigned char handler_contextual_flag = 0x02U;
inline constexpr unsigned char handler_advanced_options_flag = 0x04U;
inline constexpr std::uint64_t listener_token = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint64_t wake_token = listener_token - 1;

struct HandlerEntry {
    easy_uds::Server::Handler handler;
    // Preserve the original HandlerEntry layout: its former serialized bool
    // becomes a bit field without growing the route entry or its basic hot
    // cache footprint.
    unsigned char flags = 0;

    [[nodiscard]] bool serialized() const noexcept {
        return (flags & handler_serialized_flag) != 0;
    }

    [[nodiscard]] bool contextual() const noexcept {
        return (flags & handler_contextual_flag) != 0;
    }

    [[nodiscard]] bool advanced_options() const noexcept {
        return (flags & handler_advanced_options_flag) != 0;
    }
};

struct RouteScheduling {
    std::string domain;
    easy_uds::QueuePolicy policy = easy_uds::QueuePolicy::fifo;
};

struct SimpleRouteOptionsAdapter {
    easy_uds::RouteOptions::SimpleHandler handler;
    RouteScheduling scheduling;

    easy_uds::Response operator()(const easy_uds::Request& request) const {
        return handler(request);
    }
};

struct ContextHandlerAdapter {
    easy_uds::RouteOptions::ContextHandler handler;
    RouteScheduling scheduling;

    easy_uds::Response operator()(const easy_uds::Request&) const {
        throw std::logic_error("context handler invoked without RequestContext");
    }
};

struct StreamHandlerEntry {
    easy_uds::Server::StreamHandler handler;
};

struct HandlerRegistry {
    std::unordered_map<std::string, std::shared_ptr<const HandlerEntry>> handlers;
    std::vector<std::pair<std::string, std::shared_ptr<const HandlerEntry>>> handler_prefixes;
    std::unordered_map<std::string, std::shared_ptr<const StreamHandlerEntry>> stream_handlers;
    std::vector<std::pair<std::string, std::shared_ptr<const StreamHandlerEntry>>> stream_prefixes;
};

struct OutgoingFrame {
    protocol::HeaderBytes header{};
    std::string body;
    std::size_t offset = 0;
    Deadline deadline = Deadline::max();
};

// A connected client. fd access is governed by the reactor phase machine:
// either the reactor parses frames on it (connection in the readiness set) or a
// stream worker owns it as an exclusive lease.
struct Connection {
    Connection(int fd, peer_identity::Identity identity)
        : fd(fd),
          peer{static_cast<pid_t>(identity.pid), static_cast<uid_t>(identity.uid),
               static_cast<gid_t>(identity.gid), identity.present},
          last_io_progress(Clock::now().time_since_epoch().count()),
          last_output_progress(last_io_progress.load(std::memory_order_relaxed)) {}
    ~Connection() {
        if (fd >= 0) {
            socket_lifecycle::close(fd);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int fd = -1;
    easy_uds::PeerCredentials peer;
    std::atomic<bool> stream_active{false};  // a stream worker owns the fd (exclusive lease)
    std::atomic<bool> worker_owned{false};   // a fixed-request worker is leasing the fd
    std::atomic<bool> closing{false};
    std::atomic<bool> session_capable{false};
    std::atomic<std::size_t> active_regular{0};
    std::atomic<std::size_t> pending_serialized{0};
    std::atomic<std::size_t> inflight_requests{0};
    std::atomic<std::size_t> inflight_request_bytes{0};
    std::atomic<Clock::duration::rep> last_io_progress;
    std::atomic<Clock::duration::rep> last_output_progress;
    std::atomic<std::size_t> queued_output_bytes{0};

    // Streams hold write_mutex for their exclusive blocking exchange. Fixed
    // responses use output_mutex: a worker performs one nonblocking fast-path
    // send and the reactor drains any remainder through writable readiness.
    std::mutex write_mutex;
    std::mutex output_mutex;
    std::deque<OutgoingFrame> output_queue;
};

// Reactor-side parse state for one connection.
enum class ParsePhase { header, request_budget, request_payload, stream_budget, stream_route };

struct ReactorConnection {
    std::shared_ptr<Connection> conn;
    std::uint32_t generation = 0;
    std::uint32_t registered_events = 0;
    ParsePhase phase = ParsePhase::header;
    protocol::HeaderBytes header{};
    std::size_t header_received = 0;
    std::string route_buffer;
    std::string body_buffer;
    std::size_t payload_received = 0;
    std::size_t payload_total = 0;
    std::uint32_t request_id = 0;
    std::uint32_t arg1 = 0;
    std::uint32_t arg2 = 0;
    // Declared route+body bytes reserved against the strict aggregate request
    // budget while this frame is only partially received. Ownership transfers
    // to the queued/executing job at dispatch.
    std::size_t reserved_request_bytes = 0;
    Clock::time_point arrival_time{};        // first byte observed for current frame
    Deadline deadline = Deadline::max();     // absolute deadline once a request starts
    bool reactor_busy = false;               // guarded by connections_mutex
    bool read_paused = false;                 // guarded by connections_mutex
    std::string pending;              // bytes read ahead of the parser
    std::size_t pending_offset = 0;   // consumed prefix of pending
    // Descriptors received via ancillary data, in the order their frames
    // arrived (reactor-thread only). A frame carrying carries_fd_flag pops
    // one, and the popped descriptor is delivered to the request handler.
    std::deque<int> received_fds;
    int request_fd = -1;               // fd attached to the frame being parsed
};

struct PendingJob {
    std::shared_ptr<Connection> connection;
    easy_uds::Request request;
    Deadline deadline = Deadline::max();
    std::shared_ptr<const HandlerEntry> handler;
    bool is_stream = false;
    std::size_t request_bytes = 0;
    // Stream routes participate in the strict aggregate request budget but do
    // not increment the fixed-request count.
    bool release_stream_request_bytes = false;
    // For stream jobs: bytes already read from the socket before the lease
    // hand-off, followed by the fd.
    std::string buffered;
    // Fixed jobs need first-byte arrival while stream jobs need a buffer
    // offset, never both. Sharing their existing word keeps the regular
    // multiplexed worker-queue footprint unchanged.
    union {
        std::size_t buffered_offset = 0;
        Clock::duration::rep arrival_ticks;
    };

    [[nodiscard]] Clock::time_point fixed_arrival_time() const noexcept {
        return Clock::time_point{Clock::duration{arrival_ticks}};
    }
};

struct SerializedJob {
    std::shared_ptr<Connection> connection;  // null for maintenance jobs
    easy_uds::Request request;
    Clock::time_point arrival_time{};
    Deadline deadline = Deadline::max();
    std::shared_ptr<const HandlerEntry> handler;
    std::function<void()> maintenance;
    std::size_t request_bytes = 0;
};

struct SerializedDomainActivity {
    bool active = false;
    std::size_t queued = 0;
};

enum class SerializedAdmission {
    accepted,
    busy,
    stopping,
};

struct ServerCounterState {
    struct alignas(64) FixedRequestShard {
        std::atomic<std::uint64_t> value{0};
    };
    static constexpr std::size_t fixed_request_shard_count = 64;

    std::atomic<std::uint64_t> accepted_connections{0};
    std::atomic<std::uint64_t> rejected_connections{0};
    std::array<FixedRequestShard, fixed_request_shard_count> fixed_requests;
    std::atomic<std::uint64_t> stream_requests{0};
    std::atomic<std::uint64_t> stream_rejections{0};
    std::atomic<std::uint64_t> queue_timeouts{0};
    std::atomic<std::uint64_t> serialized_superseded{0};
    std::atomic<std::uint64_t> serialized_busy_rejections{0};
};

inline std::size_t server_counter_shard_index() noexcept {
    static std::atomic<std::size_t> next_shard{0};
    thread_local const std::size_t shard =
        next_shard.fetch_add(1, std::memory_order_relaxed) %
        ServerCounterState::fixed_request_shard_count;
    return shard;
}

struct ServerState {
    std::string socket_path;
    easy_uds::ServerOptions options;

    std::atomic<bool> running{false};
    std::atomic<std::size_t> active_streams{0};
    std::atomic<std::size_t> total_inflight_request_bytes{0};
    std::atomic<std::size_t> total_queued_output_bytes{0};
    std::atomic<std::uint32_t> next_connection_generation{1};
    std::size_t max_concurrent_streams = 1;

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    int listener_fd = -1;
    int wakeup_fd = -1;
    std::atomic<bool> wake_pending{false};
    int instance_lock_fd = -1;
    int readiness_fd = -1;
    dev_t socket_device = 0;
    ino_t socket_inode = 0;
    bool socket_identity_valid = false;
    bool stopped = false;
    bool run_started = false;
    bool run_active = false;

    // Registrations are cold-path copy-on-write updates. Request dispatch
    // atomically loads one immutable snapshot and passes shared entries to
    // workers without a global read lock or std::function copy.
    std::mutex handler_registration_mutex;
    std::shared_ptr<const HandlerRegistry> handler_registry = std::make_shared<const HandlerRegistry>();
    std::shared_ptr<const HandlerEntry> not_found_handler;

    std::mutex work_mutex;
    std::condition_variable work_cv;
    std::deque<PendingJob> pending_jobs;
    bool workers_stopping = false;

    std::mutex serialized_mutex;
    std::condition_variable serialized_cv;
    std::deque<SerializedJob> pending_serialized;
    // The legacy/default domain stays allocation-free. Named domains retain
    // one cold-path map node after first use so steady-state execution only
    // toggles a bool instead of allocating and freeing per request.
    SerializedDomainActivity default_serialized_domain_activity;
    std::unordered_map<std::string, SerializedDomainActivity>
        serialized_named_domain_activity;
    std::size_t active_serialized_domain_count = 0;
    std::size_t busy_serialized_domain_count = 0;
    std::atomic<bool> serialized_stopping{false};
    std::mutex serialized_thread_mutex;
    std::vector<std::thread> serialized_threads;
    std::atomic<std::size_t> serialized_worker_count{0};
    std::size_t max_serialized_concurrency = 1;

    std::thread reactor_thread;
    std::exception_ptr reactor_error;
    std::vector<std::thread> workers;

    // Connections by fd, keyed in the map until closed. Guarded by
    // connections_mutex; the reactor parses, while stream exchanges and the
    // short fixed-request continuation path take exclusive read leases.
    std::mutex connections_mutex;
    std::unordered_map<int, std::shared_ptr<ReactorConnection>> connections;
    std::deque<std::shared_ptr<ReactorConnection>> resumed_connections;

    // Kept at the end so the disabled option does not shift existing hot
    // bookkeeping members. Null means event recording performs no atomic RMW;
    // operational gauges reuse the accounting fields above.
    std::unique_ptr<ServerCounterState> counters;
};

inline void record_accepted_connection(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->accepted_connections.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_rejected_connection(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->rejected_connections.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_fixed_request(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->fixed_requests[server_counter_shard_index()].value.fetch_add(
            1, std::memory_order_relaxed);
    }
}

inline void record_stream_request(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->stream_requests.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_stream_rejection(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->stream_rejections.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_queue_timeout(const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->queue_timeouts.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void record_serialized_superseded(
    const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->serialized_superseded.fetch_add(
            1, std::memory_order_relaxed);
    }
}

inline void record_serialized_busy_rejection(
    const std::shared_ptr<ServerState>& state) noexcept {
    if (state->counters) {
        state->counters->serialized_busy_rejections.fetch_add(
            1, std::memory_order_relaxed);
    }
}

struct RequestContextFactory {
    static easy_uds::RequestContext make(
        const easy_uds::Request& request, Clock::time_point arrival_time,
        Deadline deadline, const std::atomic<bool>& connection_closing,
        const std::atomic<bool>& server_running) noexcept {
        return easy_uds::RequestContext(request, arrival_time, deadline,
                                        connection_closing, server_running);
    }
};

// ---- reactor subsystem boundaries ----------------------------------------

// Exact match wins; otherwise the longest registered prefix. `serialized`
// reports the executor class of the matched route.
bool find_request_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                          std::shared_ptr<const HandlerEntry>& handler);

bool find_stream_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                         std::shared_ptr<const StreamHandlerEntry>& handler);

// Moves a complete reactor-parsed frame to its executor. Fixed dispatch
// returns true when per-connection input reached its high-water mark.
bool dispatch_request(const std::shared_ptr<ServerState>& state,
                      const std::shared_ptr<ReactorConnection>& connection);
void dispatch_stream(const std::shared_ptr<ServerState>& state,
                     const std::shared_ptr<ReactorConnection>& connection);

// Runs the reactor loop (accept, frame parsing, dispatch) until stopped.
void run_reactor(const std::shared_ptr<ServerState>& state);

// Owns one streaming transaction after the reactor has leased its connection.
void run_stream_exchange(const std::shared_ptr<ServerState>& state, PendingJob&& job);

// Worker-pool loop: executes handler jobs (fixed and stream exchanges).
void worker_loop(const std::shared_ptr<ServerState>& state);

// Serialized/maintenance FIFO executor loop.
void serialized_worker_loop(const std::shared_ptr<ServerState>& state);

// Admits one serialized job and applies its domain queue policy. Reactor jobs
// can transfer request accounting atomically with queue insertion; worker
// continuation jobs pass false because they are already accounted.
SerializedAdmission enqueue_serialized_job(
    const std::shared_ptr<ServerState>& state, SerializedJob&& job,
    bool account_request, bool request_bytes_reserved);

// Joins the lazily-grown serialized executor after stop.
void join_serialized_workers(const std::shared_ptr<ServerState>& state) noexcept;

// Concurrent stream admission; false when the limit is reached.
bool try_acquire_stream_slot(const std::shared_ptr<ServerState>& state) noexcept;

// Validates and writes a response frame (500 conversion on invalid responses).
void write_fixed_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          easy_uds::Response response, std::chrono::milliseconds io_timeout,
                          Deadline deadline);

void write_error_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          std::string_view message, std::chrono::milliseconds io_timeout, Deadline deadline,
                          easy_uds::Status status = 500);

// Drains a bounded amount of queued fixed-response data without blocking.
// False marks the connection unusable (peer error, timeout, or protocol I/O).
bool flush_connection_output(const std::shared_ptr<ServerState>& state,
                             const std::shared_ptr<ReactorConnection>& connection);

// Recomputes readable/writable interest for a reactor-owned connection.
bool refresh_connection_events(const std::shared_ptr<ServerState>& state,
                               const std::shared_ptr<Connection>& connection) noexcept;

// Earliest fixed-output absolute/inactivity deadline, or Deadline::max().
Deadline connection_output_deadline(const std::shared_ptr<ServerState>& state,
                                    const std::shared_ptr<Connection>& connection);

// Per-connection input flow control. With a configured aggregate budget,
// byte accounting starts after the header and includes partially received,
// queued, and executing requests. Pause/resume toggles only readable interest.
bool try_reserve_connection_request_bytes(const std::shared_ptr<ServerState>& state,
                                          const std::shared_ptr<Connection>& connection,
                                          std::size_t request_bytes) noexcept;
void release_connection_request_bytes(const std::shared_ptr<ServerState>& state,
                                      const std::shared_ptr<Connection>& connection,
                                      std::size_t request_bytes) noexcept;
void account_connection_request(const std::shared_ptr<ServerState>& state,
                                const std::shared_ptr<Connection>& connection,
                                std::size_t request_bytes) noexcept;
void account_connection_request_count(const std::shared_ptr<Connection>& connection) noexcept;
void release_connection_request(const std::shared_ptr<ServerState>& state,
                                const std::shared_ptr<Connection>& connection,
                                std::size_t request_bytes) noexcept;
void pause_connection_reads(const std::shared_ptr<ServerState>& state,
                            const std::shared_ptr<ReactorConnection>& connection) noexcept;
bool pause_connection_reads_if_needed(const std::shared_ptr<ServerState>& state,
                                      const std::shared_ptr<ReactorConnection>& connection) noexcept;

// Enqueues a parsed request for the regular worker pool.
bool enqueue_worker_job(const std::shared_ptr<ServerState>& state, std::shared_ptr<Connection> connection,
                        easy_uds::Request request, Clock::time_point arrival_time,
                        Deadline deadline, std::shared_ptr<const HandlerEntry> handler,
                        bool is_stream, bool request_bytes_reserved, std::string buffered,
                        std::size_t buffered_offset);

// Removes and shuts down a connection; the fd closes when the last worker/
// reactor reference releases Connection, preventing fd-number reuse races.
void close_connection(const std::shared_ptr<ServerState>& state, int fd);

// Returns a worker-leased connection to the reactor with a fresh parse state.
void rearm_connection(const std::shared_ptr<ServerState>& state, const std::shared_ptr<Connection>& conn,
                      std::string buffered = {}, std::size_t buffered_offset = 0);

std::string bounded_error_body(std::string_view message, std::size_t max_message_size);

} // namespace easy_uds::detail
