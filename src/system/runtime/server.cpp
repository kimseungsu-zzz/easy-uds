#include "../reactor/core.hpp"
#include "../platform/server_path.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace easy_uds {
namespace {

using namespace detail;

// ---- stale-socket and instance-lock helpers (ported from 0.5) --------------

void verify_owned_socket_candidate(const server_path::Identity& info,
                                   const std::string& socket_path) {
    if (info.kind != server_path::EntryKind::socket) {
        throw std::runtime_error("socket path exists and is not a Unix-domain socket: " + socket_path);
    }
    if (info.owner != server_path::effective_user()) {
        throw std::runtime_error("refusing to remove a Unix-domain socket owned by another user: " + socket_path);
    }
}

FileDescriptor acquire_instance_lock(const std::string& socket_path) {
    const auto result = server_path::acquire_lock(socket_path);
    const std::string lock_path = server_path::lock_path(socket_path);
    if (result.status == server_path::LockStatus::busy) {
        throw_system_error("socket path is already owned by another easy-uds server",
                           EADDRINUSE);
    }
    if (result.status == server_path::LockStatus::invalid_entry) {
        throw std::runtime_error(
            "server lock path must be a singly-linked regular file owned by the current user: " +
            lock_path);
    }
    if (result.status != server_path::LockStatus::acquired) {
        switch (result.failure) {
        case server_path::LockFailure::open:
            throw_system_error("open server lock file failed", result.native_error);
        case server_path::LockFailure::close_on_exec:
            if (result.setup_failure == socket_lifecycle::SetupFailure::close_on_exec_getfd) {
                throw_system_error("fcntl(F_GETFD) failed", result.native_error);
            }
            throw_system_error("fcntl(F_SETFD) failed", result.native_error);
        case server_path::LockFailure::stat:
            throw_system_error("fstat server lock file failed", result.native_error);
        case server_path::LockFailure::flock:
            throw_system_error("flock server lock file failed", result.native_error);
        case server_path::LockFailure::chmod:
            throw_system_error("chmod server lock file failed", result.native_error);
        case server_path::LockFailure::none:
            break;
        }
        throw_system_error("server lock setup failed", result.native_error);
    }
    return FileDescriptor(result.fd);
}

void remove_stale_socket(const std::string& socket_path, std::chrono::milliseconds grace_period) {
    const auto before = server_path::inspect(socket_path.c_str());
    if (!before.present) {
        if (before.native_error == ENOENT) {
            return;
        }
        throw_system_error("lstat socket path failed", before.native_error);
    }
    verify_owned_socket_candidate(before.identity, socket_path);

    const Deadline grace_deadline = deadline_from_now(grace_period);
    while (true) {
        FileDescriptor probe = make_socket();
        const auto address = make_address(socket_path);
        try {
            connect_nonblocking(probe.get(), address, std::chrono::milliseconds{100}, Deadline::max());
            throw easy_uds::Error(
                easy_uds::ErrorCode::busy,
                "socket path is already in use: " + socket_path,
                {EADDRINUSE, std::generic_category()});
        } catch (const easy_uds::Error& error) {
            const int connect_error = error.system_code().value();
            if (connect_error == ENOENT) {
                return;
            }
            if (connect_error != ECONNREFUSED) {
                throw;
            }
        }

        const auto current = server_path::inspect(socket_path.c_str());
        if (!current.present) {
            if (current.native_error == ENOENT) {
                return;
            }
            throw_system_error("lstat socket path failed", current.native_error);
        }
        verify_owned_socket_candidate(current.identity, socket_path);
        if (!server_path::same_identity(before.identity, current.identity)) {
            throw std::runtime_error("socket path changed while checking whether it is stale: " + socket_path);
        }

        if (grace_period.count() == 0 || Clock::now() >= grace_deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    const auto after = server_path::inspect(socket_path.c_str());
    if (!after.present) {
        if (after.native_error == ENOENT) {
            return;
        }
        throw_system_error("lstat socket path failed", after.native_error);
    }
    verify_owned_socket_candidate(after.identity, socket_path);
    if (!server_path::same_identity(before.identity, after.identity)) {
        throw std::runtime_error("socket path changed before stale-socket removal: " + socket_path);
    }

    if (server_path::unlink_socket_path(socket_path.c_str()) != 0 && errno != ENOENT) {
        throw_system_error("remove stale socket failed", errno);
    }
}

void unlink_owned_socket(const std::shared_ptr<detail::ServerState>& state) noexcept {
    const auto current = server_path::inspect(state->socket_path.c_str());
    const server_path::Identity expected{server_path::EntryKind::socket,
                                         server_path::effective_user(),
                                         state->socket_device, state->socket_inode,
                                         0};
    if (state->socket_identity_valid && current.present &&
        server_path::is_owned_socket(current.identity, expected.owner) &&
        server_path::same_identity(current.identity, expected)) {
        (void)server_path::unlink_socket_path(state->socket_path.c_str());
    }
}

void close_lifecycle_fds_locked(const std::shared_ptr<detail::ServerState>& state) noexcept {
    if (state->listener_fd >= 0) {
        socket_lifecycle::close(state->listener_fd);
        state->listener_fd = -1;
    }
    readiness::close(state->wakeup_fd);
    state->wakeup_fd = -1;
    if (state->instance_lock_fd >= 0) {
        socket_lifecycle::close(state->instance_lock_fd);
        state->instance_lock_fd = -1;
    }
    readiness::close(state->readiness_fd);
    state->readiness_fd = -1;
}

// Signals every component to stop and wakes the reactor. Connection fds are
// shutdown (not closed) so blocked stream workers wake; run() closes them
// after all workers have joined.
void stop_state(const std::shared_ptr<detail::ServerState>& state) noexcept {
    state->running.store(false);

    bool close_without_run = false;
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        state->stopped = true;
        close_without_run = !state->run_active;
        readiness::signal(state->wakeup_fd);
    }

    unlink_owned_socket(state);

    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        for (auto& [fd, rc] : state->connections) {
            socket_lifecycle::shutdown(fd);
        }
    }

    {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        state->workers_stopping = true;
    }
    state->work_cv.notify_all();

    {
        std::lock_guard<std::mutex> lock(state->serialized_mutex);
        state->serialized_stopping.store(true);
    }
    state->serialized_cv.notify_all();

    if (close_without_run) {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (!state->run_active) {
            close_lifecycle_fds_locked(state);
        }
    }
}

class RunActiveGuard {
  public:
    explicit RunActiveGuard(std::shared_ptr<detail::ServerState> state) : state_(std::move(state)) {}

    ~RunActiveGuard() {
        state_->running.store(false);
        unlink_owned_socket(state_);
        std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
        close_lifecycle_fds_locked(state_);
        state_->run_active = false;
        state_->lifecycle_cv.notify_all();
    }

    RunActiveGuard(const RunActiveGuard&) = delete;
    RunActiveGuard& operator=(const RunActiveGuard&) = delete;

  private:
    std::shared_ptr<detail::ServerState> state_;
};

} // namespace

Server::Server(std::string socket_path, ServerOptions options) : state_(std::make_shared<detail::ServerState>()) {
    (void)make_address(socket_path);
    validate_server_options(options);

    state_->socket_path = std::move(socket_path);
    state_->options = options;
    if (options.stats == StatsMode::basic) {
        state_->counters = std::make_unique<detail::ServerCounterState>();
    }
    state_->not_found_handler = std::make_shared<const detail::HandlerEntry>(detail::HandlerEntry{
        [max_message_size = options.max_message_size](const Request&) {
            return Response{404, max_message_size >= std::string_view{"Not Found"}.size() ? "Not Found" : ""};
        },
        false});
    state_->max_concurrent_streams = options.max_concurrent_streams == 0
                                         ? std::max<std::size_t>(1, options.worker_threads - 1)
                                         : options.max_concurrent_streams;
    state_->max_serialized_concurrency =
        options.max_concurrent_serialized_domains == 0
            ? options.worker_threads
            : options.max_concurrent_serialized_domains;

    FileDescriptor instance_lock = acquire_instance_lock(state_->socket_path);
    remove_stale_socket(state_->socket_path, options.stale_socket_grace_period);

    FileDescriptor listener = make_socket();
    const auto address = make_address(state_->socket_path);
    if (platform::bind_socket(listener.get(), address) != 0) {
        throw_system_error("bind failed");
    }

    const auto identity = server_path::inspect(state_->socket_path.c_str());
    if (!identity.present) {
        throw_system_error("lstat bound socket failed", identity.native_error);
    }
    if (!server_path::is_owned_socket(identity.identity,
                                      server_path::effective_user())) {
        throw std::runtime_error("bound socket path was replaced before initialization completed");
    }
    state_->socket_device = identity.identity.device;
    state_->socket_inode = identity.identity.inode;
    state_->socket_identity_valid = true;

    if (server_path::chmod_socket_path(state_->socket_path.c_str(), options.socket_permissions) != 0) {
        const int error = errno;
        unlink_owned_socket(state_);
        throw_system_error("chmod socket path failed", error);
    }

    const auto after_chmod = server_path::inspect(state_->socket_path.c_str());
    if (!after_chmod.present ||
        !server_path::same_identity(identity.identity, after_chmod.identity) ||
        !server_path::is_owned_socket(after_chmod.identity,
                                      server_path::effective_user())) {
        unlink_owned_socket(state_);
        throw std::runtime_error("socket path changed while applying permissions");
    }

    if (platform::listen_socket(listener.get(), options.listen_backlog) != 0) {
        const int error = errno;
        unlink_owned_socket(state_);
        throw_system_error("listen failed", error);
    }

    const NativeSocket wakeup_fd = readiness::create_wakeup();
    if (wakeup_fd < 0) {
        unlink_owned_socket(state_);
        throw_system_error("wakeup event creation failed");
    }

    std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
    state_->listener_fd = listener.release();
    state_->wakeup_fd = wakeup_fd;
    state_->instance_lock_fd = instance_lock.release();
}

Server::~Server() {
    const auto state = state_;
    stop_state(state);

    std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
    state->lifecycle_cv.wait(lock, [&state] { return !state->run_active; });
}

void Server::enqueue_maintenance(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("maintenance task must not be empty");
    }
    const auto state = state_;
    detail::SerializedJob job;
    job.maintenance = std::move(task);
    if (detail::enqueue_serialized_job(state, std::move(job), false, false) !=
        detail::SerializedAdmission::accepted) {
        throw std::logic_error("server is not running");
    }
}

void Server::run() {
    const auto state = state_;

    int listener = -1;
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (state->run_started) {
            throw std::logic_error("server run() can only be called once");
        }
        state->run_started = true;
        if (state->stopped || state->listener_fd < 0 || state->wakeup_fd < 0) {
            throw std::logic_error("server has already been stopped");
        }
        state->run_active = true;
        listener = state->listener_fd;
    }

    RunActiveGuard active_guard(state);

    // Create the readiness set and register the listener + wakeup source.
    const NativeSocket readiness_fd = readiness::create_poller();
    if (readiness_fd < 0) {
        throw_system_error("readiness set creation failed");
    }
    state->readiness_fd = readiness_fd;
    if (readiness::control(readiness_fd, readiness::Control::add, listener,
                           readiness::readable, listener_token) != 0) {
        throw_system_error("readiness listener registration failed");
    }
    if (readiness::control(readiness_fd, readiness::Control::add, state->wakeup_fd,
                           readiness::readable, wake_token) != 0) {
        throw_system_error("readiness wakeup registration failed");
    }

    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (state->stopped) {
            throw std::logic_error("server was stopped during run setup");
        }
        state->running.store(true);
    }

    try {
        state->workers.reserve(state->options.worker_threads);
        for (std::size_t index = 0; index < state->options.worker_threads; ++index) {
            state->workers.emplace_back(detail::worker_loop, state);
        }
        state->reactor_thread = std::thread([state] {
            try {
                detail::run_reactor(state);
            } catch (...) {
                state->reactor_error = std::current_exception();
            }
        });
    } catch (...) {
        stop_state(state);
        for (auto& worker : state->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        detail::join_serialized_workers(state);
        throw;
    }

    // Block until the reactor exits (stop() woke it).
    state->reactor_thread.join();

    // Stop pools and wait for in-flight handlers to finish.
    stop_state(state);
    for (auto& worker : state->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    detail::join_serialized_workers(state);

    // Close every remaining connection now that no worker touches fds.
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        for (auto& [fd, rc] : state->connections) {
            rc->conn->closing.store(true, std::memory_order_release);
            socket_lifecycle::shutdown(fd);
        }
        state->connections.clear();
    }

    if (state->reactor_error) {
        std::rethrow_exception(state->reactor_error);
    }
}

void Server::stop() noexcept {
    stop_state(state_);
}

bool Server::is_running() const noexcept {
    return state_->running.load();
}

const std::string& Server::socket_path() const noexcept {
    return state_->socket_path;
}

ServerStats Server::stats() const {
    ServerStats snapshot;
    snapshot.running = state_->running.load(std::memory_order_acquire);
    snapshot.active_streams =
        state_->active_streams.load(std::memory_order_acquire);
    snapshot.retained_request_bytes =
        state_->total_inflight_request_bytes.load(std::memory_order_acquire);
    snapshot.queued_output_bytes =
        state_->total_queued_output_bytes.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> lock(state_->connections_mutex);
        snapshot.active_connections = state_->connections.size();
        for (const auto& entry : state_->connections) {
            snapshot.inflight_requests +=
                entry.second->conn->inflight_requests.load(
                    std::memory_order_acquire);
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_->work_mutex);
        snapshot.worker_queue_depth = state_->pending_jobs.size();
    }
    {
        std::lock_guard<std::mutex> lock(state_->serialized_mutex);
        snapshot.serialized_queue_depth = state_->pending_serialized.size();
        snapshot.active_serialized_domains =
            state_->active_serialized_domain_count;
    }

    if (state_->counters) {
        ServerStatsCounters counters;
        counters.accepted_connections =
            state_->counters->accepted_connections.load(std::memory_order_relaxed);
        counters.rejected_connections =
            state_->counters->rejected_connections.load(std::memory_order_relaxed);
        for (const auto& shard : state_->counters->fixed_requests) {
            counters.fixed_requests_dispatched +=
                shard.value.load(std::memory_order_relaxed);
        }
        counters.stream_requests_started =
            state_->counters->stream_requests.load(std::memory_order_relaxed);
        counters.stream_requests_rejected =
            state_->counters->stream_rejections.load(std::memory_order_relaxed);
        counters.requests_timed_out_before_execution =
            state_->counters->queue_timeouts.load(std::memory_order_relaxed);
        counters.serialized_requests_superseded =
            state_->counters->serialized_superseded.load(
                std::memory_order_relaxed);
        counters.serialized_requests_rejected_busy =
            state_->counters->serialized_busy_rejections.load(
                std::memory_order_relaxed);
        snapshot.counters = counters;
    }
    return snapshot;
}

} // namespace easy_uds
