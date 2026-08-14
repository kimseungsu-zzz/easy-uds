#include "core.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <sys/epoll.h>
#include <sys/socket.h>

namespace easy_uds::detail {

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
                          std::shared_ptr<const HandlerEntry>& handler) {
    const auto registry = std::atomic_load_explicit(&state->handler_registry, std::memory_order_acquire);
    const auto it = registry->handlers.find(route);
    if (it != registry->handlers.end()) {
        handler = it->second;
        return true;
    }
    for (const auto& entry : registry->handler_prefixes) {
        if (route.size() >= entry.first.size() && route.compare(0, entry.first.size(), entry.first) == 0) {
            handler = entry.second;
            return true;
        }
    }
    return false;
}

bool find_stream_handler(const std::shared_ptr<ServerState>& state, const std::string& route,
                         std::shared_ptr<const StreamHandlerEntry>& handler) {
    const auto registry = std::atomic_load_explicit(&state->handler_registry, std::memory_order_acquire);
    const auto it = registry->stream_handlers.find(route);
    if (it != registry->stream_handlers.end()) {
        handler = it->second;
        return true;
    }
    for (const auto& entry : registry->stream_prefixes) {
        if (route.size() >= entry.first.size() && route.compare(0, entry.first.size(), entry.first) == 0) {
            handler = entry.second;
            return true;
        }
    }
    return false;
}

bool enqueue_worker_job(const std::shared_ptr<ServerState>& state, std::shared_ptr<Connection> connection,
                        easy_uds::Request request, Clock::time_point arrival_time,
                        Deadline deadline, std::shared_ptr<const HandlerEntry> handler,
                        bool is_stream, bool request_bytes_reserved, std::string buffered,
                        std::size_t buffered_offset) {
    PendingJob job;
    job.connection = std::move(connection);
    job.request = std::move(request);
    job.arrival_time = arrival_time;
    job.deadline = deadline;
    job.handler = std::move(handler);
    job.is_stream = is_stream;
    job.request_bytes = job.request.route.size() + job.request.body.size();
    job.release_stream_request_bytes = is_stream && request_bytes_reserved;
    job.buffered = std::move(buffered);
    job.buffered_offset = buffered_offset;
    const auto accounting_connection = job.connection;
    const std::size_t accounting_bytes = job.request_bytes;
    {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        if (state->workers_stopping) {
            if (request_bytes_reserved) {
                release_connection_request_bytes(state, accounting_connection,
                                                 accounting_bytes);
            }
            return false;
        }
        try {
            state->pending_jobs.push_back(std::move(job));
        } catch (...) {
            if (request_bytes_reserved) {
                release_connection_request_bytes(state, accounting_connection,
                                                 accounting_bytes);
            }
            throw;
        }
        if (!is_stream) {
            const auto& queued = state->pending_jobs.back();
            queued.connection->active_regular.fetch_add(1, std::memory_order_relaxed);
            if (request_bytes_reserved) {
                account_connection_request_count(queued.connection);
            } else {
                account_connection_request(state, queued.connection, queued.request_bytes);
            }
        }
    }
    state->work_cv.notify_one();
    return true;
}

void close_connection(const std::shared_ptr<ServerState>& state, int fd) {
    std::unique_lock<std::mutex> lock(state->connections_mutex);
    const auto it = state->connections.find(fd);
    if (it == state->connections.end()) {
        return;
    }
    const auto connection = it->second->conn;
    const std::size_t parser_request_bytes = it->second->reserved_request_bytes;
    it->second->reserved_request_bytes = 0;
    // Descriptors that were received via SCM_RIGHTS but never delivered to a
    // handler (the frame errored or the connection died mid-parse) must not
    // outlive the connection.
    for (const int leftover : it->second->received_fds) {
        (void)::close(leftover);
    }
    it->second->received_fds.clear();
    if (it->second->request_fd >= 0) {
        (void)::close(it->second->request_fd);
        it->second->request_fd = -1;
    }
    connection->closing.store(true, std::memory_order_release);
    if (state->epoll_fd >= 0) {
        epoll_event event{};
        (void)::epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, fd, &event);
    }
    (void)::shutdown(fd, SHUT_RDWR);
    {
        std::lock_guard<std::mutex> output_lock(connection->output_mutex);
        const std::size_t queued = connection->queued_output_bytes.exchange(0, std::memory_order_acq_rel);
        connection->output_queue.clear();
        if (queued != 0) {
            state->total_queued_output_bytes.fetch_sub(queued, std::memory_order_acq_rel);
        }
    }
    if (connection->active_regular.load(std::memory_order_acquire) != 0 ||
        connection->pending_serialized.load(std::memory_order_acquire) != 0) {
        // Keep the closing connection counted against max_connections until
        // every already-dispatched response job has released its fd reference.
        lock.unlock();
        if (parser_request_bytes != 0) {
            release_connection_request_bytes(state, connection,
                                             parser_request_bytes);
        }
        return;
    }
    state->connections.erase(it);
    lock.unlock();
    if (parser_request_bytes != 0) {
        release_connection_request_bytes(state, connection, parser_request_bytes);
    }
}

bool dispatch_request(const std::shared_ptr<ServerState>& state,
                      const std::shared_ptr<ReactorConnection>& reactor_connection) {
    easy_uds::Request request;
    request.route = std::move(reactor_connection->route_buffer);
    request.body = std::move(reactor_connection->body_buffer);
    request.peer = reactor_connection->conn->peer;
    request.request_id = reactor_connection->request_id;
    request.fd = easy_uds::OwnedFd::adopt(
        std::exchange(reactor_connection->request_fd, -1));

    std::shared_ptr<const HandlerEntry> handler;
    if (!find_request_handler(state, request.route, handler)) {
        // Keep all potentially blocking response I/O off the reactor. The
        // prebuilt entry also avoids allocating a lambda for every 404.
        handler = state->not_found_handler;
    }

    if (handler->serialized()) {
        if (!ensure_serialized_worker(state)) {
            reactor_connection->conn->closing.store(true, std::memory_order_release);
            return false;
        }
        SerializedJob job;
        job.connection = reactor_connection->conn;
        job.request = std::move(request);
        job.arrival_time = reactor_connection->arrival_time;
        job.deadline = reactor_connection->deadline;
        job.handler = std::move(handler);
        job.request_bytes = job.request.route.size() + job.request.body.size();
        const bool request_bytes_reserved =
            state->options.max_total_inflight_bytes != 0;
        reactor_connection->reserved_request_bytes = 0;
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            if (state->serialized_stopping || !state->running.load()) {
                if (request_bytes_reserved) {
                    release_connection_request_bytes(state, job.connection,
                                                     job.request_bytes);
                }
                reactor_connection->conn->closing.store(true, std::memory_order_release);
                return false;
            }
            try {
                state->pending_serialized.push_back(std::move(job));
            } catch (...) {
                if (request_bytes_reserved) {
                    release_connection_request_bytes(state, reactor_connection->conn,
                                                     reactor_connection->payload_total);
                }
                throw;
            }
            reactor_connection->conn->pending_serialized.fetch_add(1, std::memory_order_relaxed);
            const auto& queued = state->pending_serialized.back();
            if (request_bytes_reserved) {
                account_connection_request_count(queued.connection);
            } else {
                account_connection_request(state, queued.connection, queued.request_bytes);
            }
        }
        state->serialized_cv.notify_one();
        return pause_connection_reads_if_needed(state, reactor_connection);
    }

    // Keep the reactor on the connection while the handler runs so later
    // multiplexed requests can be parsed and dispatched to other workers.
    if (request.request_id != 0) {
        reactor_connection->conn->session_capable.store(true, std::memory_order_relaxed);
    }
    const bool request_bytes_reserved =
        state->options.max_total_inflight_bytes != 0;
    reactor_connection->reserved_request_bytes = 0;
    if (!enqueue_worker_job(state, reactor_connection->conn, std::move(request),
                            reactor_connection->arrival_time, reactor_connection->deadline,
                            std::move(handler), false, request_bytes_reserved, {}, 0)) {
        reactor_connection->conn->closing.store(true, std::memory_order_release);
        return false;
    }
    return pause_connection_reads_if_needed(state, reactor_connection);
}

void dispatch_stream(const std::shared_ptr<ServerState>& state,
                     const std::shared_ptr<ReactorConnection>& reactor_connection) {
    const auto& connection = reactor_connection->conn;
    if (connection->active_regular.load(std::memory_order_acquire) != 0 ||
        connection->pending_serialized.load(std::memory_order_acquire) != 0) {
        connection->closing.store(true, std::memory_order_release);
        return;
    }
    if (!try_acquire_stream_slot(state)) {
        connection->closing.store(true, std::memory_order_release);
        return;
    }

    easy_uds::Request request;
    request.route = std::move(reactor_connection->route_buffer);
    request.peer = connection->peer;
    request.request_id = reactor_connection->request_id;
    const Deadline stream_deadline = reactor_connection->deadline;
    const bool request_bytes_reserved =
        state->options.max_total_inflight_bytes != 0;
    reactor_connection->reserved_request_bytes = 0;

    // Stream workers take the fd lease and carry unread reactor bytes with it.
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        if (state->epoll_fd >= 0) {
            epoll_event event{};
            (void)::epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, connection->fd, &event);
        }
        connection->stream_active.store(true, std::memory_order_release);
    }
    std::string leftover = std::move(reactor_connection->pending);
    const std::size_t leftover_offset = reactor_connection->pending_offset;
    reactor_connection->pending.clear();
    reactor_connection->pending_offset = 0;
    if (!enqueue_worker_job(state, connection, std::move(request),
                            reactor_connection->arrival_time, stream_deadline, {}, true,
                            request_bytes_reserved,
                            std::move(leftover), leftover_offset)) {
        connection->closing.store(true, std::memory_order_release);
    }
}

} // namespace easy_uds::detail
