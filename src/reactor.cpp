#include "server_core.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace easy_uds::detail {
namespace {

using protocol::HeaderBytes;
using protocol::WireType;

constexpr std::size_t kReadScratch = 64U * 1024U;
constexpr int kMaxAcceptBatch = 64;

easy_uds::Response invoke_request_handler(const easy_uds::Server::Handler& handler,
                                          const easy_uds::Request& request,
                                          const std::shared_ptr<ServerState>& state) {
    try {
        return handler(request);
    } catch (const std::exception& error) {
        const std::string_view message =
            state->options.include_handler_error_messages ? std::string_view{error.what()}
                                                          : std::string_view{"Internal Server Error"};
        return {500, bounded_error_body(message, state->options.max_message_size)};
    } catch (...) {
        return {500, bounded_error_body("Internal Server Error", state->options.max_message_size)};
    }
}

easy_uds::StreamReader bounded_error_body_reader(std::string_view message, std::size_t max_message_size) {
    std::string body = message.size() <= max_message_size ? std::string(message) : std::string{};
    return [body = std::move(body), offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
        if (offset >= body.size()) {
            return std::size_t{0};
        }
        const std::size_t take = std::min(capacity, body.size() - offset);
        std::memcpy(output, body.data() + offset, take);
        offset += take;
        return take;
    };
}

// Byte source for a stream lease: buffered bytes first, then blocking reads
// from the socket. The stream worker is the sole owner while the lease lasts.
class StreamByteSource {
  public:
    StreamByteSource(std::string& buffered, std::size_t& offset, int fd)
        : buffered_(buffered), offset_(offset), fd_(fd) {}

    void read(void* data, std::size_t size, std::chrono::milliseconds io_timeout, Deadline deadline) {
        auto* bytes = static_cast<char*>(data);
        std::size_t received = 0;
        if (offset_ < buffered_.size()) {
            const std::size_t take = std::min(size, buffered_.size() - offset_);
            std::memcpy(bytes, buffered_.data() + offset_, take);
            offset_ += take;
            received += take;
        }
        while (received < size) {
            check_absolute_deadline(deadline, "receive timed out");
            const ssize_t result = ::recv(fd_, bytes + received, size - received, 0);
            if (result > 0) {
                received += static_cast<std::size_t>(result);
                continue;
            }
            if (result == 0) {
                throw_system_error("peer closed connection", ECONNRESET);
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_for_io(fd_, POLLIN, io_timeout, deadline, "receive timed out");
                continue;
            }
            throw_system_error("receive failed");
        }
    }

    [[nodiscard]] bool buffered() const noexcept { return offset_ < buffered_.size(); }

  private:
    std::string& buffered_;
    std::size_t& offset_;
    int fd_;
};

// Incremental stream frame reader (v2). Chunk/end frames must carry the
// stream's request id; anything else is a protocol error.
class IncomingStream {
  public:
    IncomingStream(StreamByteSource& source, WireType chunk_type, WireType end_type, std::size_t max_size,
                   std::chrono::milliseconds io_timeout, Deadline deadline)
        : source_(source), chunk_type_(chunk_type), end_type_(end_type), max_size_(max_size),
          io_timeout_(io_timeout), deadline_(deadline) {}

    std::size_t read(char* buffer, std::size_t capacity) {
        if (ended_) {
            return 0;
        }
        if (frame_remaining_ == 0) {
            HeaderBytes header{};
            source_.read(header.data(), header.size(), io_timeout_, deadline_);
            const auto decoded = protocol::decode_header(header);
            if (decoded.type == end_type_) {
                if (decoded.request_id != request_id_ || decoded.arg1 != 0 || decoded.arg2 != 0) {
                    throw std::runtime_error("invalid stream end frame");
                }
                ended_ = true;
                return 0;
            }
            if (decoded.type != chunk_type_ || decoded.request_id != request_id_ || decoded.arg1 == 0 ||
                decoded.arg2 != 0) {
                throw std::runtime_error("invalid stream chunk frame");
            }
            const std::size_t chunk_size = decoded.arg1;
            if (chunk_size > std::numeric_limits<std::size_t>::max() - total_size_ ||
                (max_size_ != 0 && chunk_size > max_size_ - total_size_)) {
                throw std::length_error("stream exceeds max_stream_size");
            }
            total_size_ += chunk_size;
            frame_remaining_ = chunk_size;
        }

        const std::size_t size = std::min(capacity, frame_remaining_);
        source_.read(buffer, size, io_timeout_, deadline_);
        frame_remaining_ -= size;
        return size;
    }

    void drain(std::size_t buffer_size) {
        std::vector<char> buffer(buffer_size);
        while (read(buffer.data(), buffer.size()) != 0) {
        }
    }

    void set_request_id(std::uint32_t request_id) noexcept { request_id_ = request_id; }

  private:
    StreamByteSource& source_;
    WireType chunk_type_;
    WireType end_type_;
    std::size_t max_size_;
    std::chrono::milliseconds io_timeout_;
    Deadline deadline_;
    std::size_t total_size_ = 0;
    std::size_t frame_remaining_ = 0;
    std::uint32_t request_id_ = 0;
    bool ended_ = false;
};

class ActiveStreamGuard {
  public:
    explicit ActiveStreamGuard(std::shared_ptr<ServerState> state) : state_(std::move(state)) {}
    ~ActiveStreamGuard() { release(); }
    ActiveStreamGuard(const ActiveStreamGuard&) = delete;
    ActiveStreamGuard& operator=(const ActiveStreamGuard&) = delete;
    void release() noexcept {
        if (state_) {
            state_->active_streams.fetch_sub(1, std::memory_order_relaxed);
            state_.reset();
        }
    }

  private:
    std::shared_ptr<ServerState> state_;
};

} // namespace

bool try_acquire_stream_slot(const std::shared_ptr<ServerState>& state) noexcept {
    std::size_t active = state->active_streams.load(std::memory_order_relaxed);
    while (active < state->max_concurrent_streams) {
        if (state->active_streams.compare_exchange_weak(active, active + 1, std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

std::string bounded_error_body(std::string_view message, std::size_t max_message_size) {
    return message.size() <= max_message_size ? std::string(message) : std::string{};
}

bool find_request_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                          easy_uds::Server::Handler& handler, bool& serialized) {
    std::lock_guard<std::mutex> lock(state->handlers_mutex);
    const auto it = state->handlers.find(route);
    if (it != state->handlers.end()) {
        handler = it->second.handler;
        serialized = it->second.serialized;
        return true;
    }
    const std::pair<std::string, HandlerEntry>* best = nullptr;
    for (const auto& entry : state->handler_prefixes) {
        if (route.size() >= entry.first.size() && route.compare(0, entry.first.size(), entry.first) == 0 &&
            (best == nullptr || entry.first.size() > best->first.size())) {
            best = &entry;
        }
    }
    if (best != nullptr) {
        handler = best->second.handler;
        serialized = best->second.serialized;
        return true;
    }
    return false;
}

bool find_stream_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                         easy_uds::Server::StreamHandler& handler) {
    std::lock_guard<std::mutex> lock(state->handlers_mutex);
    const auto it = state->stream_handlers.find(route);
    if (it != state->stream_handlers.end()) {
        handler = it->second.handler;
        return true;
    }
    const std::pair<std::string, StreamHandlerEntry>* best = nullptr;
    for (const auto& entry : state->stream_prefixes) {
        if (route.size() >= entry.first.size() && route.compare(0, entry.first.size(), entry.first) == 0 &&
            (best == nullptr || entry.first.size() > best->first.size())) {
            best = &entry;
        }
    }
    if (best != nullptr) {
        handler = best->second.handler;
        return true;
    }
    return false;
}

void write_error_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          std::string_view message, std::chrono::milliseconds io_timeout, Deadline deadline,
                          easy_uds::Status status) {
    const easy_uds::Response response{status, bounded_error_body(message, state->options.max_message_size)};
    write_fixed_response(state, connection, request_id, response, io_timeout, deadline);
}

void write_fixed_response(const std::shared_ptr<ServerState>& state,
                          const std::shared_ptr<Connection>& connection, std::uint32_t request_id,
                          const easy_uds::Response& response, std::chrono::milliseconds io_timeout,
                          Deadline deadline) {
    if (response.status < 0) {
        write_error_response(state, connection, request_id,
                             state->options.include_handler_error_messages
                                 ? "response status_code must not be negative"
                                 : "Internal Server Error",
                             io_timeout, deadline);
        return;
    }
    if (response.body.size() > state->options.max_message_size ||
        response.body.size() > protocol::max_wire_field) {
        write_error_response(state, connection, request_id,
                             state->options.include_handler_error_messages ? "response exceeds max_message_size"
                                                                           : "Internal Server Error",
                             io_timeout, deadline);
        return;
    }
    std::lock_guard<std::mutex> lock(connection->write_mutex);
    write_frame_with_payload(connection->fd, WireType::response, request_id,
                             static_cast<std::uint32_t>(response.status),
                             static_cast<std::uint32_t>(response.body.size()), response.body.data(),
                             response.body.size(), io_timeout, deadline);
}

void enqueue_worker_job(const std::shared_ptr<ServerState>& state, std::shared_ptr<Connection> connection,
                        easy_uds::Request request, Deadline deadline, easy_uds::Server::Handler handler,
                        bool is_stream, std::string buffered, std::size_t buffered_offset) {
    PendingJob job;
    job.connection = std::move(connection);
    job.request = std::move(request);
    job.deadline = deadline;
    job.handler = std::move(handler);
    job.is_stream = is_stream;
    job.buffered = std::move(buffered);
    job.buffered_offset = buffered_offset;
    {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        if (state->workers_stopping) {
            return;
        }
        state->pending_jobs.push_back(std::move(job));
    }
    state->work_cv.notify_one();
}

void close_connection(const std::shared_ptr<ServerState>& state, int fd) {
    std::lock_guard<std::mutex> lock(state->connections_mutex);
    if (state->connections.find(fd) == state->connections.end()) {
        return;
    }
    state->connections.erase(fd);
    if (state->epoll_fd >= 0) {
        epoll_event ev{};
        (void)::epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, &ev);
    }
    (void)::shutdown(fd, SHUT_RDWR);
    (void)::close(fd);
}

// ---- dispatch (reactor thread) ---------------------------------------------

void dispatch_request(const std::shared_ptr<ServerState>& state, const std::shared_ptr<ReactorConnection>& rc) {
    easy_uds::Request request;
    request.route = std::move(rc->route_buffer);
    request.body = std::move(rc->body_buffer);
    request.peer = rc->conn->peer;
    request.request_id = rc->request_id;

    easy_uds::Server::Handler handler;
    bool serialized = false;
    if (!find_request_handler(state, request.route, handler, serialized)) {
        // Fast path: respond 404 directly, no worker hop.
        try {
            std::lock_guard<std::mutex> lock(rc->conn->write_mutex);
            write_frame_with_payload(rc->conn->fd, WireType::response, rc->request_id, 404, 9, "Not Found", 9,
                                     state->options.io_timeout, rc->deadline);
        } catch (...) {
            rc->conn->closing = true;
        }
        return;
    }

    if (serialized) {
        if (!ensure_serialized_worker(state)) {
            rc->conn->closing = true;
            return;
        }
        SerializedJob job;
        job.connection = rc->conn;
        job.request = std::move(request);
        job.deadline = rc->deadline;
        job.handler = std::move(handler);
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            if (state->serialized_stopping || !state->running.load()) {
                return;
            }
            state->pending_serialized.push_back(std::move(job));
        }
        state->serialized_cv.notify_one();
        return;
    }

    // Lease the connection to the serving worker so it can keep reading
    // follow-up requests directly (no reactor hop per request); the worker
    // re-arms it with the reactor after the idle grace elapses.
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        if (state->epoll_fd >= 0) {
            epoll_event ev{};
            (void)::epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, rc->conn->fd, &ev);
        }
        rc->conn->worker_owned = true;
    }
    std::string leftover = std::move(rc->pending);
    const std::size_t leftover_offset = rc->pending_offset;
    rc->pending.clear();
    rc->pending_offset = 0;
    enqueue_worker_job(state, rc->conn, std::move(request), rc->deadline, std::move(handler), false,
                       std::move(leftover), leftover_offset);
}

void dispatch_stream(const std::shared_ptr<ServerState>& state, const std::shared_ptr<ReactorConnection>& rc) {
    if (!try_acquire_stream_slot(state)) {
        // Excess stream: reject explicitly by closing the connection.
        rc->conn->closing = true;
        return;
    }

    easy_uds::Request request;
    request.route = std::move(rc->route_buffer);
    request.peer = rc->conn->peer;
    request.request_id = rc->request_id;

    const Deadline stream_deadline = earlier_deadline(rc->deadline, deadline_from_now(state->options.stream_timeout));

    // The stream worker takes over the fd; unread buffered bytes ride along.
    // Remove it from epoll before handing off so rearm can add it exactly once.
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        if (state->epoll_fd >= 0) {
            epoll_event ev{};
            (void)::epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, rc->conn->fd, &ev);
        }
        rc->conn->stream_active.store(true, std::memory_order_release);
    }
    std::string leftover = std::move(rc->pending);
    const std::size_t leftover_offset = rc->pending_offset;
    rc->pending.clear();
    rc->pending_offset = 0;
    enqueue_worker_job(state, rc->conn, std::move(request), stream_deadline, {}, true, std::move(leftover),
                       leftover_offset);
}

// ---- stream exchange (worker-owned lease) ----------------------------------

void run_stream_exchange(const std::shared_ptr<ServerState>& state, PendingJob&& job) {
    auto conn = job.connection;
    const int fd = conn->fd;
    ActiveStreamGuard stream_guard(state);
    try {
        StreamByteSource source(job.buffered, job.buffered_offset, fd);
        IncomingStream incoming(source, WireType::stream_request_chunk, WireType::stream_request_end,
                                state->options.max_stream_size, state->options.io_timeout, job.deadline);
        incoming.set_request_id(job.request.request_id);
        StreamReader body_reader = [&incoming](char* buffer, std::size_t capacity) {
            return incoming.read(buffer, capacity);
        };

        StreamResponse response;
        easy_uds::Server::StreamHandler handler;
        if (!find_stream_handler(state, job.request.route, handler)) {
            response.status = 404;
        } else {
            try {
                response = handler(body_reader, job.request);
            } catch (const std::exception& error) {
                response = state->options.include_handler_error_messages
                               ? StreamResponse{500, bounded_error_body_reader(error.what(),
                                                                               state->options.max_message_size)}
                               : StreamResponse{500, {}};
            } catch (...) {
                response = {500, {}};
            }
        }

        // A handler may inspect only a prefix; consume the rest so the
        // half-duplex exchange cannot deadlock.
        incoming.drain(state->options.stream_chunk_size);

        if (response.status < 0) {
            response = {500, {}};
        }
        {
            std::lock_guard<std::mutex> lock(conn->write_mutex);
            write_header_frame(fd, WireType::stream_response, job.request.request_id,
                               static_cast<std::uint32_t>(response.status), 0, state->options.io_timeout,
                               job.deadline);
            std::vector<char> chunk_buffer(state->options.stream_chunk_size);
            std::size_t total_size = 0;
            while (response.body) {
                const std::size_t size = response.body(chunk_buffer.data(), chunk_buffer.size());
                if (size == 0) {
                    break;
                }
                if (size > chunk_buffer.size() ||
                    size > std::numeric_limits<std::size_t>::max() - total_size ||
                    (state->options.max_stream_size != 0 &&
                     size > state->options.max_stream_size - total_size)) {
                    throw std::length_error("stream exceeds max_stream_size");
                }
                total_size += size;
                write_frame_with_payload(fd, WireType::stream_response_chunk, job.request.request_id,
                                         static_cast<std::uint32_t>(size), 0, chunk_buffer.data(), size,
                                         state->options.io_timeout, job.deadline);
            }
            write_header_frame(fd, WireType::stream_response_end, job.request.request_id, 0, 0,
                               state->options.io_timeout, job.deadline);
        }
    } catch (...) {
        // Peer closed, timed out, sent malformed frames, or the server
        // stopped. The connection is done; drop the lease quietly.
        conn->closing.store(true, std::memory_order_release);
    }

    // Lease ends: hand the connection back to the reactor (or close on stop).
    rearm_connection(state, conn, std::move(job.buffered), job.buffered_offset);
}

// ---- worker pool -----------------------------------------------------------

// Hands a leased connection back to the reactor with a fresh parse state.
// Called by a worker; run()'s cleanup handles connections when not running.
void rearm_connection(const std::shared_ptr<ServerState>& state, const std::shared_ptr<Connection>& conn,
                      std::string buffered, std::size_t buffered_offset) {
    const int fd = conn->fd;
    if (conn->closing.load(std::memory_order_acquire)) {
        close_connection(state, fd);
        return;
    }
    if (!state->running.load()) {
        return;
    }
    auto fresh = std::make_shared<ReactorConnection>();
    fresh->conn = conn;
    fresh->pending = std::move(buffered);
    fresh->pending_offset = buffered_offset;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        state->connections[fd] = fresh;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        conn->closing.store(true, std::memory_order_release);
        close_connection(state, fd);
        return;
    }
    conn->stream_active.store(false, std::memory_order_release);
    conn->worker_owned.store(false, std::memory_order_release);
}

// Serves one parsed fixed request from the worker side: 404 / serialized
// hand-off / inline invocation and response write.
void serve_fixed_request(const std::shared_ptr<ServerState>& state, const std::shared_ptr<Connection>& conn,
                         easy_uds::Request& request, Deadline deadline) {
    easy_uds::Server::Handler handler;
    bool serialized = false;
    if (!find_request_handler(state, request.route, handler, serialized)) {
        try {
            std::lock_guard<std::mutex> lock(conn->write_mutex);
            write_frame_with_payload(conn->fd, WireType::response, request.request_id, 404, 9, "Not Found", 9,
                                     state->options.io_timeout, deadline);
        } catch (...) {
            conn->closing = true;
        }
        return;
    }
    if (serialized) {
        if (!ensure_serialized_worker(state)) {
            conn->closing = true;
            return;
        }
        SerializedJob job;
        job.connection = conn;
        job.request = std::move(request);
        job.deadline = deadline;
        job.handler = std::move(handler);
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            if (state->serialized_stopping || !state->running.load()) {
                return;
            }
            state->pending_serialized.push_back(std::move(job));
        }
        state->serialized_cv.notify_one();
        return;
    }

    const easy_uds::Response response = invoke_request_handler(handler, request, state);
    try {
        write_fixed_response(state, conn, request.request_id, response, state->options.io_timeout, deadline);
    } catch (...) {
        conn->closing = true;
    }
}

// A worker that just served a fixed request keeps reading the leased
// connection directly, serving follow-up requests inline, until the peer
// pauses longer than session_idle_grace (or closes the connection); it then
// returns the connection to the reactor.
void continue_connection(const std::shared_ptr<ServerState>& state, std::shared_ptr<Connection> conn,
                         std::string buffered, std::size_t buffered_offset) {
    const int fd = conn->fd;
    const std::chrono::milliseconds grace = state->options.session_idle_grace;
    StreamByteSource source(buffered, buffered_offset, fd);
    bool request_started = false;
    try {
        while (state->running.load() && !conn->closing.load(std::memory_order_acquire)) {
            // Apply the short grace only while no byte of the next request is
            // available. Once a header starts, use the normal request deadline
            // so a fragmented header is never discarded during rearm.
            request_started = source.buffered();
            if (!request_started) {
                if (grace.count() == 0) {
                    break;
                }
                wait_for_io(fd, POLLIN, std::chrono::milliseconds{0}, deadline_from_now(grace),
                            "receive timed out");
                request_started = true;
            }
            const Deadline req_deadline = deadline_from_now(state->options.request_timeout);
            HeaderBytes header{};
            source.read(header.data(), header.size(), state->options.io_timeout, req_deadline);
            const auto decoded = protocol::decode_header(header);
            if (decoded.type != WireType::request) {
                throw std::runtime_error("unexpected frame on persistent connection");
            }
            protocol::validate_request_lengths(decoded.arg1, decoded.arg2, state->options.max_message_size);

            easy_uds::Request request;
            request.route.resize(decoded.arg1);
            request.body.resize(decoded.arg2);
            if (decoded.arg1 != 0) {
                source.read(request.route.data(), request.route.size(), state->options.io_timeout, req_deadline);
            }
            if (decoded.arg2 != 0) {
                source.read(request.body.data(), request.body.size(), state->options.io_timeout, req_deadline);
            }
            request.peer = conn->peer;
            request.request_id = decoded.request_id;

            serve_fixed_request(state, conn, request, req_deadline);
            request_started = false;
        }
    } catch (const std::system_error& error) {
        if (error.code().value() != ETIMEDOUT || request_started) {
            conn->closing.store(true, std::memory_order_release);  // peer closed or request I/O failure
        }
        // ETIMEDOUT: the idle grace elapsed; hand back to the reactor.
    } catch (...) {
        conn->closing.store(true, std::memory_order_release);  // malformed frame or protocol error
    }
    rearm_connection(state, conn, std::move(buffered), buffered_offset);
}

void worker_loop(const std::shared_ptr<ServerState>& state) {
    while (true) {
        PendingJob job;
        {
            std::unique_lock<std::mutex> lock(state->work_mutex);
            state->work_cv.wait(lock, [&state] {
                return state->workers_stopping || !state->pending_jobs.empty();
            });
            if (state->pending_jobs.empty()) {
                if (state->workers_stopping) {
                    return;
                }
                continue;
            }
            job = std::move(state->pending_jobs.front());
            state->pending_jobs.pop_front();
        }

        if (job.is_stream) {
            run_stream_exchange(state, std::move(job));
            continue;
        }

        try {
            if (Clock::now() >= job.deadline) {
                write_error_response(state, job.connection, job.request.request_id,
                                     "request timed out before execution", state->options.io_timeout,
                                     Deadline::max(), 408);
                rearm_connection(state, job.connection);
                continue;
            }
            if (!state->running.load()) {
                rearm_connection(state, job.connection);
                continue;
            }
            const easy_uds::Response response = invoke_request_handler(job.handler, job.request, state);
            try {
                write_fixed_response(state, job.connection, job.request.request_id, response,
                                     state->options.io_timeout, job.deadline);
            } catch (const std::exception&) {
                job.connection->closing = true;
            } catch (...) {
                job.connection->closing = true;
            }
        } catch (...) {
            job.connection->closing = true;
        }

        // The request was served on a leased connection: keep reading
        // follow-up requests inline, then re-arm the reactor when idle.
        continue_connection(state, job.connection, std::move(job.buffered), job.buffered_offset);
    }
}

// ---- serialized executor ---------------------------------------------------

void serialized_worker_loop(const std::shared_ptr<ServerState>& state) {
    while (true) {
        SerializedJob job;
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            state->serialized_cv.wait(lock, [&state] {
                return state->serialized_stopping || !state->pending_serialized.empty();
            });
            if (state->pending_serialized.empty()) {
                if (state->serialized_stopping) {
                    return;
                }
                continue;
            }
            job = std::move(state->pending_serialized.front());
            state->pending_serialized.pop_front();
        }

        if (job.maintenance) {
            try {
                job.maintenance();
            } catch (...) {
                // A failed maintenance task must never take the executor down.
            }
            continue;
        }

        try {
            if (Clock::now() >= job.deadline) {
                write_error_response(state, job.connection, job.request.request_id,
                                     "request timed out before execution", state->options.io_timeout,
                                     Deadline::max(), 408);
                continue;
            }
            if (!state->running.load()) {
                continue;
            }
            const easy_uds::Response response = invoke_request_handler(job.handler, job.request, state);
            try {
                write_fixed_response(state, job.connection, job.request.request_id, response,
                                     state->options.io_timeout, job.deadline);
            } catch (...) {
            }
        } catch (...) {
        }
    }
}

bool ensure_serialized_worker(const std::shared_ptr<ServerState>& state) {
    std::lock_guard<std::mutex> lock(state->serialized_thread_mutex);
    if (state->serialized_thread.joinable()) {
        return true;
    }
    if (!state->running.load() || state->serialized_stopping.load()) {
        return false;
    }
    state->serialized_thread = std::thread(serialized_worker_loop, state);
    return true;
}

void join_serialized_worker(const std::shared_ptr<ServerState>& state) noexcept {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(state->serialized_thread_mutex);
        worker = std::move(state->serialized_thread);
    }
    if (worker.joinable()) {
        worker.join();
    }
}

// ---- reactor loop ----------------------------------------------------------

// Reads available bytes and parses complete frames, dispatching requests.
// Returns normally; the caller closes the connection on a protocol error.
void consume(const std::shared_ptr<ServerState>& state, const std::shared_ptr<ReactorConnection>& rc) {
    const int fd = rc->conn->fd;
    std::array<char, kReadScratch> scratch{};

    while (true) {
        const ssize_t n = ::recv(fd, scratch.data(), scratch.size(), 0);
        if (n > 0) {
            // Compact the consumed prefix occasionally so pending stays bounded
            // by one in-flight frame.
            if (rc->pending_offset != 0 && rc->pending_offset == rc->pending.size()) {
                rc->pending.clear();
                rc->pending_offset = 0;
            }
            rc->pending.append(scratch.data(), static_cast<std::size_t>(n));
            break;
        }
        if (n == 0) {
            rc->conn->closing = true;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        rc->conn->closing = true;
        return;
    }

    while (true) {
        const std::size_t available = rc->pending.size() - rc->pending_offset;
        if (rc->phase == ParsePhase::header) {
            const std::size_t need = protocol::header_size - rc->header_received;
            if (available < need) {
                return;
            }
            std::memcpy(rc->header.data() + rc->header_received, rc->pending.data() + rc->pending_offset, need);
            rc->pending_offset += need;
            rc->header_received = 0;

            const auto decoded = protocol::decode_header(rc->header);
            if (decoded.type == WireType::request) {
                protocol::validate_request_lengths(decoded.arg1, decoded.arg2, state->options.max_message_size);
                rc->request_id = decoded.request_id;
                rc->arg1 = decoded.arg1;
                rc->arg2 = decoded.arg2;
                rc->payload_total = static_cast<std::size_t>(decoded.arg1) + decoded.arg2;
                rc->payload_received = 0;
                rc->route_buffer.clear();
                rc->body_buffer.clear();
                rc->route_buffer.reserve(decoded.arg1);
                rc->body_buffer.reserve(decoded.arg2);
                rc->phase = ParsePhase::request_payload;
                rc->deadline = deadline_from_now(state->options.request_timeout);
                continue;
            }
            if (decoded.type == WireType::stream_request) {
                if (decoded.arg1 == 0 || decoded.arg2 != 0 ||
                    decoded.arg1 > state->options.max_message_size) {
                    throw std::runtime_error("invalid stream request header");
                }
                rc->request_id = decoded.request_id;
                rc->arg1 = decoded.arg1;
                rc->route_buffer.clear();
                rc->route_buffer.reserve(decoded.arg1);
                rc->phase = ParsePhase::stream_route;
                rc->deadline = deadline_from_now(state->options.request_timeout);
                continue;
            }
            throw std::runtime_error("unexpected protocol message type");
        }

        if (rc->phase == ParsePhase::request_payload) {
            const std::size_t remaining = rc->payload_total - rc->payload_received;
            const std::size_t take = std::min(available, remaining);
            if (take != 0) {
                const std::size_t route_part = std::min(take, rc->arg1 - rc->route_buffer.size());
                rc->route_buffer.append(rc->pending.data() + rc->pending_offset, route_part);
                rc->body_buffer.append(rc->pending.data() + rc->pending_offset + route_part, take - route_part);
                rc->pending_offset += take;
                rc->payload_received += take;
            }
            if (rc->payload_received == rc->payload_total) {
                dispatch_request(state, rc);
                rc->phase = ParsePhase::header;
                continue;
            }
            return;
        }

        // ParsePhase::stream_route
        const std::size_t take = std::min(available, rc->arg1 - rc->route_buffer.size());
        rc->route_buffer.append(rc->pending.data() + rc->pending_offset, take);
        rc->pending_offset += take;
        if (rc->route_buffer.size() == rc->arg1) {
            dispatch_stream(state, rc);
        }
        return;
    }
}

void run_reactor(const std::shared_ptr<ServerState>& state) {
    const int listener = state->listener_fd;
    const int wake_read = state->wake_read_fd;
    const int epoll_fd = state->epoll_fd;

    std::array<epoll_event, 128> events{};
    while (state->running.load()) {
        const int count = ::epoll_wait(epoll_fd, events.data(), static_cast<int>(events.size()), -1);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!state->running.load()) {
                break;
            }
            throw_system_error("epoll_wait failed");
        }

        for (int index = 0; index < count; ++index) {
            const int fd = events[index].data.fd;
            const std::uint32_t mask = events[index].events;

            if (fd == listener) {
                if ((mask & (EPOLLERR | EPOLLHUP)) != 0) {
                    break;  // listening socket failed; treat as fatal
                }
                // Drain a bounded batch of accepted connections.
                std::size_t accepted = 0;
                while (accepted < kMaxAcceptBatch) {
#if defined(__linux__) && defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
                    const int client_fd = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
                    const int client_fd = ::accept(listener, nullptr, nullptr);
#endif
                    if (client_fd < 0) {
                        if (errno == EINTR || errno == ECONNABORTED) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        break;
                    }
                    ++accepted;
                    {
                        std::lock_guard<std::mutex> lock(state->connections_mutex);
                        if (state->connections.size() >= state->options.max_connections) {
                            (void)::close(client_fd);
                            continue;
                        }
                        auto conn = std::make_shared<Connection>(client_fd, capture_peer_credentials(client_fd));
                        auto rc = std::make_shared<ReactorConnection>();
                        rc->conn = conn;
                        state->connections[client_fd] = rc;
                        epoll_event ev{};
                        ev.events = EPOLLIN;
                        ev.data.fd = client_fd;
                        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) != 0) {
                            state->connections.erase(client_fd);
                            (void)::close(client_fd);
                        }
                    }
                }
                continue;
            }

            if (fd == wake_read) {
                std::array<unsigned char, 64> drain{};
                while (::read(wake_read, drain.data(), drain.size()) > 0) {
                }
                // Fall through below the loop; the next run() cleanup handles
                // the remaining connections.
                index = count;  // stop processing further events
                break;
            }

            std::shared_ptr<ReactorConnection> rc;
            {
                std::lock_guard<std::mutex> lock(state->connections_mutex);
                const auto it = state->connections.find(fd);
                if (it == state->connections.end()) {
                    continue;
                }
                rc = it->second;
            }
            if (rc->conn->stream_active.load(std::memory_order_acquire) ||
                rc->conn->worker_owned.load(std::memory_order_acquire) ||
                rc->conn->closing.load(std::memory_order_acquire)) {
                continue;
            }
            if ((mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                close_connection(state, fd);
                continue;
            }
            if ((mask & EPOLLIN) != 0) {
                try {
                    consume(state, rc);
                } catch (...) {
                    // Malformed or invalid frames: isolate the peer.
                    rc->conn->closing = true;
                }
                if (rc->conn->closing.load(std::memory_order_acquire)) {
                    close_connection(state, fd);
                }
            }
        }
    }
}

} // namespace easy_uds::detail
