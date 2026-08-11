#pragma once

// Server-side core types shared by the reactor, the worker pool, and the
// public Server implementation.

#include "easy_uds/easy_uds.hpp"
#include "internal.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
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

// A connected client. fd access is governed by the reactor phase machine:
// either the reactor parses frames on it (connection in the epoll set) or a
// stream worker owns it as an exclusive lease.
struct Connection {
    Connection(int fd, easy_uds::PeerCredentials peer) : fd(fd), peer(peer) {}

    int fd = -1;
    easy_uds::PeerCredentials peer;
    std::atomic<bool> stream_active{false};  // a stream worker owns the fd (exclusive lease)
    std::atomic<bool> worker_owned{false};   // a fixed-request worker is leasing the fd
    std::atomic<bool> closing{false};

    // Responses are written as complete frames under this lock so multiplexed
    // responses never interleave inside a frame.
    std::mutex write_mutex;
};

// Reactor-side parse state for one connection.
enum class ParsePhase { header, request_payload, stream_route };

struct ReactorConnection {
    std::shared_ptr<Connection> conn;
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
    Deadline deadline = Deadline::max();
    std::string pending;              // bytes read ahead of the parser
    std::size_t pending_offset = 0;   // consumed prefix of pending
};

struct PendingJob {
    std::shared_ptr<Connection> connection;
    easy_uds::Request request;
    Deadline deadline = Deadline::max();
    easy_uds::Server::Handler handler;
    bool is_stream = false;
    // For stream jobs: bytes already read from the socket before the lease
    // hand-off, followed by the fd.
    std::string buffered;
    std::size_t buffered_offset = 0;
};

struct SerializedJob {
    std::shared_ptr<Connection> connection;  // null for maintenance jobs
    easy_uds::Request request;
    Deadline deadline = Deadline::max();
    easy_uds::Server::Handler handler;
    std::function<void()> maintenance;
};

struct ServerState {
    std::string socket_path;
    easy_uds::ServerOptions options;

    std::atomic<bool> running{false};
    std::atomic<std::size_t> active_streams{0};
    std::size_t max_concurrent_streams = 1;

    std::mutex lifecycle_mutex;
    std::condition_variable lifecycle_cv;
    int listener_fd = -1;
    int wake_read_fd = -1;
    int wake_write_fd = -1;
    int instance_lock_fd = -1;
    int epoll_fd = -1;
    bool stopped = false;
    bool run_started = false;
    bool run_active = false;

    std::mutex handlers_mutex;
    std::unordered_map<std::string, HandlerEntry> handlers;
    std::vector<std::pair<std::string, HandlerEntry>> handler_prefixes;
    std::unordered_map<std::string, StreamHandlerEntry> stream_handlers;
    std::vector<std::pair<std::string, StreamHandlerEntry>> stream_prefixes;

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
    std::vector<std::thread> workers;

    // Connections by fd, keyed in the map until closed. Guarded by
    // connections_mutex; the reactor parses, stream workers lease.
    std::mutex connections_mutex;
    std::unordered_map<int, std::shared_ptr<ReactorConnection>> connections;
};

// ---- shared helpers implemented in reactor.cpp ----------------------------

// Exact match wins; otherwise the longest registered prefix. `serialized`
// reports the executor class of the matched route.
bool find_request_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                          easy_uds::Server::Handler& handler, bool& serialized);

bool find_stream_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                         easy_uds::Server::StreamHandler& handler);

// Runs the reactor loop (accept, frame parsing, dispatch) until stopped.
void run_reactor(const std::shared_ptr<ServerState>& state);

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
                          const easy_uds::Response& response, std::chrono::milliseconds io_timeout,
                          Deadline deadline);

void write_error_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          std::string_view message, std::chrono::milliseconds io_timeout, Deadline deadline,
                          easy_uds::Status status = 500);

// Enqueues a parsed request for the regular worker pool.
void enqueue_worker_job(const std::shared_ptr<ServerState>& state, std::shared_ptr<Connection> connection,
                        easy_uds::Request request, Deadline deadline, easy_uds::Server::Handler handler,
                        bool is_stream, std::string buffered, std::size_t buffered_offset);

// Shuts down and closes a connection; idempotent (map ownership based).
void close_connection(const std::shared_ptr<ServerState>& state, int fd);

// Returns a worker-leased connection to the reactor with a fresh parse state.
void rearm_connection(const std::shared_ptr<ServerState>& state, const std::shared_ptr<Connection>& conn,
                      std::string buffered = {}, std::size_t buffered_offset = 0);

std::string bounded_error_body(std::string_view message, std::size_t max_message_size);

} // namespace easy_uds::detail
