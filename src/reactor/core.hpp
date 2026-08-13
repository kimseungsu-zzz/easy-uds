#pragma once

// Server-side core types shared by the reactor, the worker pool, and the
// public Server implementation.

#include "easy_uds/easy_uds.hpp"
#include "../internal.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace easy_uds::detail {

struct HandlerEntry {
    easy_uds::Server::Handler handler;
    bool serialized = false;
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
// either the reactor parses frames on it (connection in the epoll set) or a
// stream worker owns it as an exclusive lease.
struct Connection {
    Connection(int fd, easy_uds::PeerCredentials peer)
        : fd(fd), peer(peer), last_io_progress(Clock::now().time_since_epoch().count()),
          last_output_progress(last_io_progress.load(std::memory_order_relaxed)) {}
    ~Connection() {
        if (fd >= 0) {
            (void)::close(fd);
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
    // send and the reactor drains any remainder through EPOLLOUT.
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
    Deadline deadline = Deadline::max();     // absolute deadline once a request starts
    bool reactor_busy = false;               // guarded by connections_mutex
    bool read_paused = false;                 // guarded by connections_mutex
    std::string pending;              // bytes read ahead of the parser
    std::size_t pending_offset = 0;   // consumed prefix of pending
    // Descriptors received via SCM_RIGHTS, in the order their frames arrived
    // (reactor-thread only). A frame carrying carries_fd_flag pops one, and
    // the popped descriptor is delivered to the request handler.
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
    std::size_t buffered_offset = 0;
};

struct SerializedJob {
    std::shared_ptr<Connection> connection;  // null for maintenance jobs
    easy_uds::Request request;
    Deadline deadline = Deadline::max();
    std::shared_ptr<const HandlerEntry> handler;
    std::function<void()> maintenance;
    std::size_t request_bytes = 0;
};

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
    int wake_read_fd = -1;
    int wake_write_fd = -1;
    std::atomic<bool> wake_pending{false};
    int instance_lock_fd = -1;
    int epoll_fd = -1;
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
    std::atomic<bool> serialized_stopping{false};
    std::mutex serialized_thread_mutex;
    std::thread serialized_thread;

    std::thread reactor_thread;
    std::exception_ptr reactor_error;
    std::vector<std::thread> workers;

    // Connections by fd, keyed in the map until closed. Guarded by
    // connections_mutex; the reactor parses, while stream exchanges and the
    // short fixed-request continuation path take exclusive read leases.
    std::mutex connections_mutex;
    std::unordered_map<int, std::shared_ptr<ReactorConnection>> connections;
    std::deque<std::shared_ptr<ReactorConnection>> resumed_connections;
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

// Starts the serialized executor on first use; false when not running.
bool ensure_serialized_worker(const std::shared_ptr<ServerState>& state);

// Joins the lazily-created serialized executor after stop.
void join_serialized_worker(const std::shared_ptr<ServerState>& state) noexcept;

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

// Recomputes EPOLLIN/EPOLLOUT interest for a reactor-owned connection.
bool refresh_connection_events(const std::shared_ptr<ServerState>& state,
                               const std::shared_ptr<Connection>& connection) noexcept;

// Earliest fixed-output absolute/inactivity deadline, or Deadline::max().
Deadline connection_output_deadline(const std::shared_ptr<ServerState>& state,
                                    const std::shared_ptr<Connection>& connection);

// Per-connection input flow control. With a configured aggregate budget,
// byte accounting starts after the header and includes partially received,
// queued, and executing requests. Pause/resume toggles only EPOLLIN.
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
                        easy_uds::Request request, Deadline deadline, std::shared_ptr<const HandlerEntry> handler,
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
