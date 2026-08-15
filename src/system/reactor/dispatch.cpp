#include "core.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
                        RequestCapabilityStorage capabilities, bool is_stream,
                        bool request_bytes_reserved, std::string buffered,
                        std::size_t buffered_offset) {
    PendingJob job;
    job.connection = std::move(connection);
    job.request = std::move(request);
    job.capabilities = std::move(capabilities);
    job.deadline = deadline;
    job.handler = std::move(handler);
    job.is_stream = is_stream;
    job.request_bytes = job.request.route.size() + job.request.body.size();
    job.release_stream_request_bytes = is_stream && request_bytes_reserved;
    job.buffered = std::move(buffered);
    if (is_stream) {
        job.buffered_offset = buffered_offset;
    } else {
        job.arrival_ticks = arrival_time.time_since_epoch().count();
    }
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

void close_connection(const std::shared_ptr<ServerState>& state, NativeSocket fd) {
    std::unique_lock<std::mutex> lock(state->connections_mutex);
    const auto it = state->connections.find(fd);
    if (it == state->connections.end()) {
        return;
    }
    const auto connection = it->second->conn;
    const std::size_t parser_request_bytes = it->second->reserved_request_bytes;
    it->second->reserved_request_bytes = 0;
    // Descriptors that were received via ancillary data but never delivered to
    // a handler (the frame errored or the connection died mid-parse) must not
    // outlive the connection.
    it->second->received_fds.clear();
    it->second->capabilities = {};
    connection->closing.store(true, std::memory_order_release);
    if (state->readiness_fd >= 0) {
        (void)readiness::control(state->readiness_fd,
                                 readiness::Control::remove, fd, 0, 0);
    }
    socket_lifecycle::shutdown(fd);
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
    record_fixed_request(state);
    easy_uds::Request request;
    request.route = std::move(reactor_connection->route_buffer);
    request.body = std::move(reactor_connection->body_buffer);
    request.request_id = reactor_connection->request_id;
    reactor_connection->capabilities.peer = reactor_connection->conn->peer;

    std::shared_ptr<const HandlerEntry> handler;
    if (!find_request_handler(state, request.route, handler)) {
        // Keep all potentially blocking response I/O off the reactor. The
        // prebuilt entry also avoids allocating a lambda for every 404.
        handler = state->not_found_handler;
    }

    if (handler->serialized()) {
        SerializedJob job;
        job.connection = reactor_connection->conn;
        job.request = std::move(request);
        job.capabilities = std::move(reactor_connection->capabilities);
        job.arrival_time = reactor_connection->arrival_time;
        job.deadline = reactor_connection->deadline;
        job.handler = std::move(handler);
        job.request_bytes = job.request.route.size() + job.request.body.size();
        const bool request_bytes_reserved =
            state->options.max_total_inflight_bytes != 0;
        reactor_connection->reserved_request_bytes = 0;
        const std::uint32_t request_id = job.request.request_id;
        SerializedAdmission admission;
        try {
            admission = enqueue_serialized_job(
                state, std::move(job), true, request_bytes_reserved);
        } catch (...) {
            if (request_bytes_reserved) {
                release_connection_request_bytes(
                    state, reactor_connection->conn,
                    reactor_connection->payload_total);
            }
            throw;
        }
        if (admission == SerializedAdmission::busy) {
            if (request_bytes_reserved) {
                release_connection_request_bytes(
                    state, reactor_connection->conn,
                    reactor_connection->payload_total);
            }
            write_error_response(state, reactor_connection->conn, request_id,
                                 "serialization domain is busy",
                                 state->options.io_timeout, Deadline::max(),
                                 easy_uds::status_conflict);
            return pause_connection_reads_if_needed(state,
                                                     reactor_connection);
        }
        if (admission == SerializedAdmission::stopping) {
            if (request_bytes_reserved) {
                release_connection_request_bytes(
                    state, reactor_connection->conn,
                    reactor_connection->payload_total);
            }
            reactor_connection->conn->closing.store(true,
                                                     std::memory_order_release);
            return false;
        }
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
                            std::move(handler),
                            std::move(reactor_connection->capabilities), false,
                            request_bytes_reserved, {}, 0)) {
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
        record_stream_rejection(state);
        connection->closing.store(true, std::memory_order_release);
        return;
    }
    record_stream_request(state);

    easy_uds::Request request;
    request.route = std::move(reactor_connection->route_buffer);
    request.request_id = reactor_connection->request_id;
    RequestCapabilityStorage capabilities;
    capabilities.peer = connection->peer;
    const Deadline stream_deadline = reactor_connection->deadline;
    const bool request_bytes_reserved =
        state->options.max_total_inflight_bytes != 0;
    reactor_connection->reserved_request_bytes = 0;

    // Stream workers take the fd lease and carry unread reactor bytes with it.
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        if (state->readiness_fd >= 0) {
            (void)readiness::control(state->readiness_fd,
                                     readiness::Control::remove,
                                     connection->fd, 0, 0);
        }
        connection->stream_active.store(true, std::memory_order_release);
    }
    std::string leftover = std::move(reactor_connection->pending);
    const std::size_t leftover_offset = reactor_connection->pending_offset;
    reactor_connection->pending.clear();
    reactor_connection->pending_offset = 0;
    if (!enqueue_worker_job(state, connection, std::move(request),
                            reactor_connection->arrival_time, stream_deadline, {},
                            std::move(capabilities), true, request_bytes_reserved,
                            std::move(leftover), leftover_offset)) {
        connection->closing.store(true, std::memory_order_release);
    }
}

} // namespace easy_uds::detail
