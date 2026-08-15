#include "core.hpp"

#include "common.hpp"
#include "stream_io.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace easy_uds::detail {
namespace {

using protocol::HeaderBytes;
using protocol::WireType;

const RouteScheduling& route_scheduling(
    const std::shared_ptr<const HandlerEntry>& handler) {
    static const RouteScheduling default_scheduling{};
    if (!handler || !handler->advanced_options()) {
        return default_scheduling;
    }
    if (handler->contextual()) {
        const auto* adapter = handler->handler.target<ContextHandlerAdapter>();
        if (adapter == nullptr) {
            throw std::logic_error("invalid contextual route options entry");
        }
        return adapter->scheduling;
    }
    const auto* adapter = handler->handler.target<SimpleRouteOptionsAdapter>();
    if (adapter == nullptr) {
        throw std::logic_error("invalid simple route options entry");
    }
    return adapter->scheduling;
}

const std::string& serialized_job_domain(const SerializedJob& job) {
    return route_scheduling(job.handler).domain;
}

easy_uds::Response invoke_request_handler(
    const std::shared_ptr<const HandlerEntry>& handler,
    const easy_uds::Request& request,
    const RequestCapabilityStorage& capabilities,
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
                state->running, capabilities);
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
                         RequestCapabilityStorage capabilities,
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
        SerializedJob job;
        job.connection = connection;
        job.request = std::move(request);
        job.capabilities = std::move(capabilities);
        job.arrival_time = arrival_time;
        job.deadline = deadline;
        job.handler = std::move(handler);
        job.request_bytes = job.request.route.size() + job.request.body.size();
        const std::uint32_t request_id = job.request.request_id;
        SerializedAdmission admission;
        try {
            admission = enqueue_serialized_job(
                state, std::move(job), false, false);
        } catch (...) {
            // The continuation request was accounted before this call. Return
            // it to complete_regular_request instead of leaking the count or
            // retained-byte budget on executor allocation failure.
            connection->closing.store(true, std::memory_order_release);
            return true;
        }
        if (admission == SerializedAdmission::busy) {
            write_error_response(state, connection, request_id,
                                 "serialization domain is busy",
                                 state->options.io_timeout, Deadline::max(),
                                 easy_uds::status_conflict);
            return true;
        }
        if (admission == SerializedAdmission::stopping) {
            connection->closing.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    easy_uds::Response response = invoke_request_handler(
        handler, request, capabilities, state, connection, arrival_time,
        deadline);
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

    if (readiness::control(state->readiness_fd, readiness::Control::remove,
                           connection->fd, 0, 0) != 0 &&
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
                wait_for_io(fd, socket_wait::Interest::read,
                            std::chrono::milliseconds{0}, deadline_from_now(grace),
                            "receive timed out");
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
            request.request_id = decoded.request_id;
            RequestCapabilityStorage capabilities;
            capabilities.peer = connection->peer;
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
                serve_fixed_request(state, connection, request,
                                    std::move(capabilities), arrival_time,
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
            existing->second->received_fds.clear();
            existing->second->capabilities = {};
        }

        fresh->conn = connection;
        fresh->generation = allocate_connection_generation(state);
        fresh->registered_events = readiness::readable;
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

        if (readiness::control(state->readiness_fd, readiness::Control::add, fd,
                               fresh->registered_events,
                               connection_token(fd, fresh->generation)) != 0) {
            const int add_error = errno;
            if (add_error != EEXIST ||
                readiness::control(state->readiness_fd,
                                   readiness::Control::modify, fd,
                                   fresh->registered_events,
                                   connection_token(fd, fresh->generation)) != 0) {
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
                    invoke_request_handler(job.handler, job.request,
                                           job.capabilities, state,
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

namespace {

const SerializedDomainActivity* find_serialized_domain_activity(
    const ServerState& state, const std::string& domain) {
    if (domain.empty()) {
        return &state.default_serialized_domain_activity;
    }
    const auto found = state.serialized_named_domain_activity.find(domain);
    return found == state.serialized_named_domain_activity.end()
               ? nullptr
               : &found->second;
}

SerializedDomainActivity& serialized_domain_activity(
    ServerState& state, const std::string& domain) {
    if (domain.empty()) {
        return state.default_serialized_domain_activity;
    }
    return state.serialized_named_domain_activity.try_emplace(domain).first
        ->second;
}

bool serialized_domain_active(const ServerState& state,
                              const std::string& domain) {
    const SerializedDomainActivity* activity =
        find_serialized_domain_activity(state, domain);
    return activity && activity->active;
}

bool serialized_domain_busy(const ServerState& state,
                            const std::string& domain) {
    const SerializedDomainActivity* activity =
        find_serialized_domain_activity(state, domain);
    return activity && (activity->active || activity->queued != 0);
}

void activate_serialized_domain(ServerState& state,
                                SerializedDomainActivity& activity) {
    activity.active = true;
    ++state.active_serialized_domain_count;
}

void deactivate_serialized_domain(ServerState& state,
                                  SerializedDomainActivity& activity) {
    if (activity.active) {
        activity.active = false;
        --state.active_serialized_domain_count;
        if (activity.queued == 0) {
            --state.busy_serialized_domain_count;
        }
    }
}

bool has_runnable_serialized_job(const ServerState& state) {
    return std::any_of(
        state.pending_serialized.begin(), state.pending_serialized.end(),
        [&state](const SerializedJob& job) {
            return !serialized_domain_active(state,
                                             serialized_job_domain(job));
        });
}

bool ensure_serialized_worker_count(
    const std::shared_ptr<ServerState>& state, std::size_t desired) {
    std::lock_guard<std::mutex> lock(state->serialized_thread_mutex);
    if (!state->running.load(std::memory_order_acquire) ||
        state->serialized_stopping.load(std::memory_order_acquire)) {
        return false;
    }
    desired = std::min(desired, state->max_serialized_concurrency);
    while (state->serialized_threads.size() < desired) {
        state->serialized_threads.emplace_back(serialized_worker_loop, state);
        state->serialized_worker_count.store(
            state->serialized_threads.size(), std::memory_order_release);
    }
    return true;
}

std::size_t desired_serialized_worker_count(
    const ServerState& state, const std::string& incoming_domain) {
    const std::size_t current = state.serialized_worker_count.load(
        std::memory_order_acquire);
    std::size_t demand = state.busy_serialized_domain_count;
    if (!serialized_domain_busy(state, incoming_domain)) {
        ++demand;
    }
    return std::max(current,
                    std::min(demand, state.max_serialized_concurrency));
}

void finish_superseded_job(const std::shared_ptr<ServerState>& state,
                           SerializedJob& job) noexcept {
    record_serialized_superseded(state);
    try {
        write_error_response(state, job.connection, job.request.request_id,
                             "superseded by a newer request",
                             state->options.io_timeout, Deadline::max(),
                             easy_uds::status_conflict);
    } catch (...) {
        job.connection->closing.store(true, std::memory_order_release);
    }
    job.connection->pending_serialized.fetch_sub(1,
                                                  std::memory_order_release);
    release_connection_request(state, job.connection, job.request_bytes);
    wake_reactor(state);
}

struct SerializedDomainGuard {
    std::shared_ptr<ServerState> state;
    SerializedDomainActivity* activity;

    ~SerializedDomainGuard() {
        {
            std::lock_guard<std::mutex> lock(state->serialized_mutex);
            deactivate_serialized_domain(*state, *activity);
        }
        state->serialized_cv.notify_all();
    }
};

} // namespace

SerializedAdmission enqueue_serialized_job(
    const std::shared_ptr<ServerState>& state, SerializedJob&& job,
    bool account_request, bool request_bytes_reserved) {
    const RouteScheduling& scheduling = route_scheduling(job.handler);
    std::vector<SerializedJob> superseded;
    {
        std::lock_guard<std::mutex> lock(state->serialized_mutex);
        if (state->serialized_stopping.load(std::memory_order_acquire) ||
            !state->running.load(std::memory_order_acquire)) {
            return SerializedAdmission::stopping;
        }

        // Materialize the named-domain state before queue insertion. This is
        // the only potentially allocating domain-state operation, so an
        // allocation failure cannot leave a queued but unaccounted request.
        SerializedDomainActivity& incoming_activity =
            serialized_domain_activity(*state, scheduling.domain);
        const std::size_t desired_workers =
            desired_serialized_worker_count(*state, scheduling.domain);
        // serialized_mutex -> serialized_thread_mutex is the only nested
        // scheduler lock order. Newly created workers wait on
        // serialized_mutex, so publication and admission stay one decision.
        if (!ensure_serialized_worker_count(state, desired_workers) ||
            !state->running.load(std::memory_order_acquire)) {
            return SerializedAdmission::stopping;
        }

        const auto same_domain = [&scheduling](const SerializedJob& queued) {
            return serialized_job_domain(queued) == scheduling.domain;
        };
        if (scheduling.policy == easy_uds::QueuePolicy::reject_if_busy &&
            serialized_domain_busy(*state, scheduling.domain)) {
            record_serialized_busy_rejection(state);
            return SerializedAdmission::busy;
        }

        if (scheduling.policy == easy_uds::QueuePolicy::latest_wins) {
            const std::size_t replacement_count = static_cast<std::size_t>(
                std::count_if(
                    state->pending_serialized.begin(),
                    state->pending_serialized.end(),
                    [&](const SerializedJob& queued) {
                        return same_domain(queued) && queued.connection &&
                               queued.request.route == job.request.route;
                    }));
            superseded.reserve(replacement_count);
        }

        state->pending_serialized.push_back(std::move(job));
        if (!incoming_activity.active && incoming_activity.queued == 0) {
            ++state->busy_serialized_domain_count;
        }
        ++incoming_activity.queued;
        if (scheduling.policy == easy_uds::QueuePolicy::latest_wins) {
            for (std::size_t index = 0;
                 index + 1 < state->pending_serialized.size();) {
                auto& queued = state->pending_serialized[index];
                if (same_domain(queued) && queued.connection &&
                    queued.request.route ==
                        state->pending_serialized.back().request.route) {
                    superseded.push_back(std::move(queued));
                    state->pending_serialized.erase(
                        state->pending_serialized.begin() +
                        static_cast<std::ptrdiff_t>(index));
                    --incoming_activity.queued;
                } else {
                    ++index;
                }
            }
        }

        const auto& admitted = state->pending_serialized.back();
        if (admitted.connection) {
            admitted.connection->pending_serialized.fetch_add(
                1, std::memory_order_relaxed);
            if (account_request) {
                if (request_bytes_reserved) {
                    account_connection_request_count(admitted.connection);
                } else {
                    account_connection_request(
                        state, admitted.connection, admitted.request_bytes);
                }
            }
        }
    }

    for (auto& replaced : superseded) {
        finish_superseded_job(state, replaced);
    }
    state->serialized_cv.notify_all();
    return SerializedAdmission::accepted;
}

void serialized_worker_loop(const std::shared_ptr<ServerState>& state) {
    while (true) {
        SerializedJob job;
        SerializedDomainActivity* activity = nullptr;
        {
            std::unique_lock<std::mutex> lock(state->serialized_mutex);
            state->serialized_cv.wait(lock, [&state] {
                return state->serialized_stopping.load(
                           std::memory_order_acquire) ||
                       has_runnable_serialized_job(*state);
            });
            const auto next = std::find_if(
                state->pending_serialized.begin(),
                state->pending_serialized.end(),
                [&state](const SerializedJob& queued) {
                    return !serialized_domain_active(
                        *state, serialized_job_domain(queued));
                });
            if (next == state->pending_serialized.end()) {
                if (state->serialized_stopping.load(
                        std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            const std::string& domain = serialized_job_domain(*next);
            activity = &serialized_domain_activity(*state, domain);
            --activity->queued;
            activate_serialized_domain(*state, *activity);
            job = std::move(*next);
            state->pending_serialized.erase(next);
        }
        SerializedDomainGuard domain_guard{state, activity};

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
                connection->pending_serialized.fetch_sub(
                    1, std::memory_order_release);
                release_connection_request(state, connection, request_bytes);
                wake_reactor(state);
            }
        } completion{state, job.connection, job.request_bytes};

        try {
            if (Clock::now() >= job.deadline) {
                record_queue_timeout(state);
                write_error_response(state, job.connection,
                                     job.request.request_id,
                                     "request timed out before execution",
                                     state->options.io_timeout,
                                     Deadline::max(),
                                     easy_uds::status_request_timeout);
                continue;
            }
            if (!state->running.load()) {
                continue;
            }
            easy_uds::Response response = invoke_request_handler(
                job.handler, job.request, job.capabilities, state, job.connection,
                job.arrival_time, job.deadline);
            try {
                write_fixed_response(state, job.connection,
                                     job.request.request_id,
                                     std::move(response),
                                     state->options.io_timeout,
                                     job.deadline);
            } catch (...) {
                job.connection->closing.store(true,
                                               std::memory_order_release);
            }
        } catch (...) {
            job.connection->closing.store(true, std::memory_order_release);
        }
    }
}

void join_serialized_workers(
    const std::shared_ptr<ServerState>& state) noexcept {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(state->serialized_thread_mutex);
        workers = std::move(state->serialized_threads);
        state->serialized_worker_count.store(0, std::memory_order_release);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

} // namespace easy_uds::detail
