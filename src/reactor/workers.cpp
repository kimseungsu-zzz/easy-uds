#include "core.hpp"

#include "common.hpp"
#include "stream_io.hpp"

#include <cerrno>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

namespace easy_uds::detail {
namespace {

using protocol::HeaderBytes;
using protocol::WireType;

easy_uds::Response invoke_request_handler(
    const std::shared_ptr<const HandlerEntry>& handler,
    const easy_uds::Request& request,
    const std::shared_ptr<ServerState>& state,
    const std::shared_ptr<Connection>& connection,
    Clock::time_point arrival_time, Deadline deadline) {
    try {
        if (handler->contextual()) {
            const auto* adapter =
                handler->handler.target<ContextHandlerAdapter>();
            if (adapter == nullptr) {
                throw std::logic_error("invalid context handler entry");
            }
            const auto context = RequestContextFactory::make(
                request, arrival_time, deadline, connection->closing,
                state->running);
            return adapter->handler(request, context);
        }
        return handler->handler(request);
    } catch (const std::exception& error) {
        const std::string_view message = state->options.include_handler_error_messages
                                             ? std::string_view{error.what()}
                                             : std::string_view{"Internal Server Error"};
        return {500, bounded_error_body(message, state->options.max_message_size)};
    } catch (...) {
        return {500, bounded_error_body("Internal Server Error",
                                        state->options.max_message_size)};
    }
}

bool serve_fixed_request(const std::shared_ptr<ServerState>& state,
                         const std::shared_ptr<Connection>& connection,
                         easy_uds::Request& request,
                         Clock::time_point arrival_time, Deadline deadline) {
    std::shared_ptr<const HandlerEntry> handler;
    if (!find_request_handler(state, request.route, handler)) {
        try {
            write_fixed_response(
                state, connection, request.request_id,
                {404, bounded_error_body("Not Found", state->options.max_message_size)},
                state->options.io_timeout, deadline);
        } catch (...) {
            connection->closing.store(true, std::memory_order_release);
        }
        return true;
    }
    if (handler->serialized()) {
        if (!ensure_serialized_worker(state)) {
            connection->closing.store(true, std::memory_order_release);
            return true;
        }
        SerializedJob job;
        job.connection = connection;
        job.request = std::move(request);
        job.arrival_time = arrival_time;
        job.deadline = deadline;
        job.handler = std::move(handler);
        job.request_bytes = job.request.route.size() + job.request.body.size();
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            if (state->serialized_stopping || !state->running.load()) {
                connection->closing.store(true, std::memory_order_release);
                return true;
            }
            try {
                state->pending_serialized.push_back(std::move(job));
            } catch (...) {
                throw;
            }
            connection->pending_serialized.fetch_add(1, std::memory_order_relaxed);
        }
        state->serialized_cv.notify_one();
        return false;
    }

    easy_uds::Response response = invoke_request_handler(
        handler, request, state, connection, arrival_time, deadline);
    try {
        write_fixed_response(state, connection, request.request_id, std::move(response),
                             state->options.io_timeout, deadline);
    } catch (...) {
        connection->closing.store(true, std::memory_order_release);
    }
    return true;
}

bool try_acquire_continuation_lease(const std::shared_ptr<ServerState>& state,
                                    const std::shared_ptr<Connection>& connection) {
    // A configured aggregate request budget has one strict admission point in
    // the reactor, before parser buffers are reserved. Keep the default
    // zero-budget fast path unchanged, but do not bypass that admission point
    // through a worker-owned continuation read.
    if (state->options.max_total_inflight_bytes != 0 || !state->running.load() ||
        !connection->session_capable.load(std::memory_order_relaxed) ||
        connection->closing.load(std::memory_order_acquire) ||
        connection->active_regular.load(std::memory_order_acquire) != 0 ||
        connection->pending_serialized.load(std::memory_order_acquire) != 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state->connections_mutex);
    const auto it = state->connections.find(connection->fd);
    if (it == state->connections.end() || it->second->conn != connection ||
        it->second->reactor_busy || it->second->phase != ParsePhase::header ||
        it->second->read_paused ||
        it->second->pending_offset != it->second->pending.size() ||
        connection->stream_active.load(std::memory_order_acquire) ||
        connection->worker_owned.load(std::memory_order_acquire) ||
        connection->closing.load(std::memory_order_acquire) ||
        connection->active_regular.load(std::memory_order_acquire) != 0 ||
        connection->pending_serialized.load(std::memory_order_acquire) != 0) {
        return false;
    }

    epoll_event event{};
    if (::epoll_ctl(state->epoll_fd, EPOLL_CTL_DEL, connection->fd, &event) != 0 &&
        errno != ENOENT) {
        connection->closing.store(true, std::memory_order_release);
        return false;
    }
    connection->worker_owned.store(true, std::memory_order_release);
    return true;
}

bool complete_regular_request(const std::shared_ptr<ServerState>& state,
                              const std::shared_ptr<Connection>& connection,
                              std::size_t request_bytes,
                              bool release_accounting = true) {
    const std::size_t previous =
        connection->active_regular.fetch_sub(1, std::memory_order_acq_rel);
    if (release_accounting) {
        release_connection_request(state, connection, request_bytes);
    }
    if (previous != 1) {
        return false;
    }
    if (connection->closing.load(std::memory_order_acquire)) {
        wake_reactor(state);
        return false;
    }
    return try_acquire_continuation_lease(state, connection);
}

void continue_connection(const std::shared_ptr<ServerState>& state,
                         std::shared_ptr<Connection> connection,
                         std::string buffered, std::size_t buffered_offset) {
    const int fd = connection->fd;
    const std::chrono::milliseconds grace = state->options.session_idle_grace;
    while (state->running.load() &&
           !connection->closing.load(std::memory_order_acquire)) {
        StreamByteSource source(buffered, buffered_offset, fd);
        bool request_started = false;
        try {
            request_started = source.buffered();
            if (!request_started) {
                if (grace.count() == 0) {
                    break;
                }
                wait_for_io(fd, POLLIN, std::chrono::milliseconds{0},
                            deadline_from_now(grace), "receive timed out");
                request_started = true;
            }
            const Clock::time_point arrival_time = Clock::now();
            const Deadline request_deadline =
                deadline_from(arrival_time, state->options.request_timeout);
            HeaderBytes header{};
            source.read(header.data(), header.size(), state->options.io_timeout,
                        request_deadline);
            const auto decoded = protocol::decode_header(header);
            if (decoded.flags & protocol::carries_fd_flag) {
                throw std::runtime_error("fd flag is not supported on persistent-session requests");
            }
            if (decoded.type == WireType::stream_request) {
                std::string replay(reinterpret_cast<const char*>(header.data()),
                                   header.size());
                replay.append(buffered.data() + buffered_offset,
                              buffered.size() - buffered_offset);
                rearm_connection(state, connection, std::move(replay), 0);
                return;
            }
            if (decoded.type != WireType::request) {
                throw std::runtime_error(
                    "unexpected frame on persistent connection");
            }
            protocol::validate_request_lengths(decoded.arg1, decoded.arg2,
                                               state->options.max_message_size);

            easy_uds::Request request;
            request.route.resize(decoded.arg1);
            request.body.resize(decoded.arg2);
            if (decoded.arg1 != 0) {
                source.read(request.route.data(), request.route.size(),
                            state->options.io_timeout, request_deadline);
            }
            if (decoded.arg2 != 0) {
                source.read(request.body.data(), request.body.size(),
                            state->options.io_timeout, request_deadline);
            }
            request.peer = connection->peer;
            request.request_id = decoded.request_id;
            const std::size_t request_bytes =
                request.route.size() + request.body.size();

            record_fixed_request(state);
            connection->active_regular.fetch_add(1, std::memory_order_relaxed);
            account_connection_request(state, connection, request_bytes);
            if (request.request_id != 0) {
                connection->session_capable.store(true, std::memory_order_relaxed);
            }
            rearm_connection(state, connection, std::move(buffered), buffered_offset);
            const bool completed_inline =
                serve_fixed_request(state, connection, request, arrival_time,
                                    request_deadline);
            if (!complete_regular_request(state, connection, request_bytes,
                                          completed_inline)) {
                return;
            }
            buffered.clear();
            buffered_offset = 0;
        } catch (const easy_uds::Error& error) {
            if (error.kind() != easy_uds::ErrorCode::timeout || request_started) {
                connection->closing.store(true, std::memory_order_release);
            }
            rearm_connection(state, connection, std::move(buffered), buffered_offset);
            return;
        } catch (...) {
            connection->closing.store(true, std::memory_order_release);
            rearm_connection(state, connection, std::move(buffered), buffered_offset);
            return;
        }
    }
    rearm_connection(state, connection, std::move(buffered), buffered_offset);
}

} // namespace

void rearm_connection(const std::shared_ptr<ServerState>& state,
                      const std::shared_ptr<Connection>& connection,
                      std::string buffered, std::size_t buffered_offset) {
    const int fd = connection->fd;
    if (connection->closing.load(std::memory_order_acquire)) {
        close_connection(state, fd);
        return;
    }
    if (!state->running.load()) {
        return;
    }
    const bool may_reuse =
        connection->worker_owned.load(std::memory_order_acquire) &&
        !connection->stream_active.load(std::memory_order_acquire);
    std::shared_ptr<ReactorConnection> fresh;
    if (!may_reuse) {
        fresh = std::make_shared<ReactorConnection>();
    }
    mark_io_progress(connection);
    bool rearm_failed = false;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        const auto existing = state->connections.find(fd);
        if (may_reuse && existing != state->connections.end() &&
            existing->second->conn == connection && !existing->second->reactor_busy) {
            fresh = existing->second;
        } else if (!fresh) {
            fresh = std::make_shared<ReactorConnection>();
        }

        // Replacing a parse state must not leak descriptors that were received
        // but never delivered to a handler.
        if (existing != state->connections.end() && fresh != existing->second) {
            for (const int leftover : existing->second->received_fds) {
                (void)::close(leftover);
            }
            existing->second->received_fds.clear();
            if (existing->second->request_fd >= 0) {
                (void)::close(existing->second->request_fd);
                existing->second->request_fd = -1;
            }
        }

        fresh->conn = connection;
        fresh->generation = allocate_connection_generation(state);
        fresh->registered_events = EPOLLIN;
        fresh->phase = ParsePhase::header;
        fresh->header.fill(0);
        fresh->header_received = 0;
        clear_reusable_buffer(fresh->route_buffer);
        clear_reusable_buffer(fresh->body_buffer);
        fresh->payload_received = 0;
        fresh->payload_total = 0;
        fresh->request_id = 0;
        fresh->arg1 = 0;
        fresh->arg2 = 0;
        fresh->reserved_request_bytes = 0;
        fresh->arrival_time = Clock::time_point{};
        fresh->deadline = Deadline::max();
        fresh->reactor_busy = false;
        fresh->read_paused = false;
        clear_reusable_buffer(fresh->pending);
        if (buffered_offset < buffered.size()) {
            fresh->pending.append(buffered.data() + buffered_offset,
                                  buffered.size() - buffered_offset);
        }
        fresh->pending_offset = 0;
        state->connections[fd] = fresh;
        connection->stream_active.store(false, std::memory_order_release);
        connection->worker_owned.store(false, std::memory_order_release);

        epoll_event event{};
        event.events = fresh->registered_events;
        event.data.u64 = connection_token(fd, fresh->generation);
        if (::epoll_ctl(state->epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
            const int add_error = errno;
            if (add_error != EEXIST ||
                ::epoll_ctl(state->epoll_fd, EPOLL_CTL_MOD, fd, &event) != 0) {
                connection->closing.store(true, std::memory_order_release);
                rearm_failed = true;
            }
        }
    }
    if (rearm_failed) {
        close_connection(state, fd);
    }
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
                record_queue_timeout(state);
                write_error_response(state, job.connection, job.request.request_id,
                                     "request timed out before execution",
                                     state->options.io_timeout, Deadline::max(), 408);
            } else if (state->running.load()) {
                easy_uds::Response response =
                    invoke_request_handler(job.handler, job.request, state,
                                           job.connection,
                                           job.fixed_arrival_time(),
                                           job.deadline);
                try {
                    write_fixed_response(state, job.connection, job.request.request_id,
                                         std::move(response), state->options.io_timeout,
                                         job.deadline);
                } catch (...) {
                    job.connection->closing.store(true, std::memory_order_release);
                }
            }
        } catch (...) {
            job.connection->closing.store(true, std::memory_order_release);
        }
        if (complete_regular_request(state, job.connection, job.request_bytes)) {
            // Fixed reactor jobs never carry leased stream bytes; the union
            // word holds arrival ticks for this executor class.
            continue_connection(state, job.connection, std::move(job.buffered), 0);
        }
    }
}

void serialized_worker_loop(const std::shared_ptr<ServerState>& state) {
    while (true) {
        SerializedJob job;
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            state->serialized_cv.wait(lock, [&state] {
                return state->serialized_stopping ||
                       !state->pending_serialized.empty();
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
            }
            continue;
        }

        struct CompletionGuard {
            std::shared_ptr<ServerState> state;
            std::shared_ptr<Connection> connection;
            std::size_t request_bytes;
            ~CompletionGuard() {
                connection->pending_serialized.fetch_sub(1,
                                                         std::memory_order_release);
                release_connection_request(state, connection, request_bytes);
                wake_reactor(state);
            }
        } completion{state, job.connection, job.request_bytes};

        try {
            if (Clock::now() >= job.deadline) {
                record_queue_timeout(state);
                write_error_response(state, job.connection, job.request.request_id,
                                     "request timed out before execution",
                                     state->options.io_timeout, Deadline::max(), 408);
                continue;
            }
            if (!state->running.load()) {
                continue;
            }
            easy_uds::Response response =
                invoke_request_handler(job.handler, job.request, state,
                                       job.connection, job.arrival_time,
                                       job.deadline);
            try {
                write_fixed_response(state, job.connection, job.request.request_id,
                                     std::move(response), state->options.io_timeout,
                                     job.deadline);
            } catch (...) {
                job.connection->closing.store(true, std::memory_order_release);
            }
        } catch (...) {
            job.connection->closing.store(true, std::memory_order_release);
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

} // namespace easy_uds::detail
