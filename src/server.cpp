#include "server_core.hpp"

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

#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace easy_uds {
namespace {

using namespace detail;

// ---- stale-socket and instance-lock helpers (ported from 0.5) --------------

bool same_file_identity(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

void verify_owned_socket_candidate(const struct stat& info, const std::string& socket_path) {
    if (!S_ISSOCK(info.st_mode)) {
        throw std::runtime_error("socket path exists and is not a Unix-domain socket: " + socket_path);
    }
    if (info.st_uid != ::geteuid()) {
        throw std::runtime_error("refusing to remove a Unix-domain socket owned by another user: " + socket_path);
    }
}

std::string instance_lock_path(const std::string& socket_path) {
    return socket_path + ".lock";
}

FileDescriptor acquire_instance_lock(const std::string& socket_path) {
    const std::string lock_path = instance_lock_path(socket_path);
    int flags = O_RDWR | O_CREAT;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif

    FileDescriptor fd(::open(lock_path.c_str(), flags, 0600));
    if (fd.get() < 0) {
        throw_system_error("open server lock file failed");
    }
    set_close_on_exec(fd.get());

    struct stat info {};
    if (::fstat(fd.get(), &info) != 0) {
        throw_system_error("fstat server lock file failed");
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1) {
        throw std::runtime_error(
            "server lock path must be a singly-linked regular file owned by the current user: " + lock_path);
    }
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        if (error == EWOULDBLOCK || error == EAGAIN) {
            throw_system_error("socket path is already owned by another easy-uds server", EADDRINUSE);
        }
        throw_system_error("flock server lock file failed", error);
    }
    if (::fchmod(fd.get(), 0600) != 0) {
        throw_system_error("chmod server lock file failed");
    }
    return fd;
}

void remove_stale_socket(const std::string& socket_path, std::chrono::milliseconds grace_period) {
    struct stat before {};
    if (::lstat(socket_path.c_str(), &before) != 0) {
        if (errno == ENOENT) {
            return;
        }
        throw_system_error("lstat socket path failed");
    }
    verify_owned_socket_candidate(before, socket_path);

    const Deadline grace_deadline = deadline_from_now(grace_period);
    while (true) {
        FileDescriptor probe = make_socket();
        const sockaddr_un address = make_address(socket_path);
        try {
            connect_nonblocking(probe.get(), address, std::chrono::milliseconds{100}, Deadline::max());
            throw std::runtime_error("socket path is already in use: " + socket_path);
        } catch (const std::system_error& error) {
            const int connect_error = error.code().value();
            if (connect_error == ENOENT) {
                return;
            }
            if (connect_error != ECONNREFUSED) {
                throw;
            }
        }

        struct stat current {};
        if (::lstat(socket_path.c_str(), &current) != 0) {
            if (errno == ENOENT) {
                return;
            }
            throw_system_error("lstat socket path failed");
        }
        verify_owned_socket_candidate(current, socket_path);
        if (!same_file_identity(before, current)) {
            throw std::runtime_error("socket path changed while checking whether it is stale: " + socket_path);
        }

        if (grace_period.count() == 0 || Clock::now() >= grace_deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    struct stat after {};
    if (::lstat(socket_path.c_str(), &after) != 0) {
        if (errno == ENOENT) {
            return;
        }
        throw_system_error("lstat socket path failed");
    }
    verify_owned_socket_candidate(after, socket_path);
    if (!same_file_identity(before, after)) {
        throw std::runtime_error("socket path changed before stale-socket removal: " + socket_path);
    }

    if (::unlink(socket_path.c_str()) != 0 && errno != ENOENT) {
        throw_system_error("remove stale socket failed");
    }
}

std::array<FileDescriptor, 2> make_wakeup_pipe() {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        throw_system_error("pipe failed");
    }
    std::array<FileDescriptor, 2> result{FileDescriptor(fds[0]), FileDescriptor(fds[1])};
    set_close_on_exec(result[0].get());
    set_close_on_exec(result[1].get());
    set_nonblocking(result[0].get());
    set_nonblocking(result[1].get());
    return result;
}

void unlink_owned_socket(const std::shared_ptr<detail::ServerState>& state) noexcept {
    struct stat current {};
    if (::lstat(state->socket_path.c_str(), &current) == 0 && S_ISSOCK(current.st_mode) &&
        current.st_uid == ::geteuid()) {
        (void)::unlink(state->socket_path.c_str());
    }
}

void close_lifecycle_fds_locked(const std::shared_ptr<detail::ServerState>& state) noexcept {
    if (state->listener_fd >= 0) {
        (void)::close(state->listener_fd);
        state->listener_fd = -1;
    }
    if (state->wake_read_fd >= 0) {
        (void)::close(state->wake_read_fd);
        state->wake_read_fd = -1;
    }
    if (state->wake_write_fd >= 0) {
        (void)::close(state->wake_write_fd);
        state->wake_write_fd = -1;
    }
    if (state->instance_lock_fd >= 0) {
        (void)::close(state->instance_lock_fd);
        state->instance_lock_fd = -1;
    }
    if (state->epoll_fd >= 0) {
        (void)::close(state->epoll_fd);
        state->epoll_fd = -1;
    }
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
        if (state->wake_write_fd >= 0) {
            const unsigned char byte = 1;
            const ssize_t result = ::write(state->wake_write_fd, &byte, sizeof(byte));
            (void)result;
        }
    }

    unlink_owned_socket(state);

    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        for (auto& [fd, rc] : state->connections) {
            (void)::shutdown(fd, SHUT_RDWR);
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

// Validates a route registration argument.
void validate_route(std::string_view route, std::size_t max_message_size) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > max_message_size) {
        throw std::length_error("route exceeds server max_message_size");
    }
}

} // namespace

Server::Server(std::string socket_path, ServerOptions options) : state_(std::make_shared<detail::ServerState>()) {
    (void)make_address(socket_path);
    validate_server_options(options);

    state_->socket_path = std::move(socket_path);
    state_->options = options;
    state_->max_concurrent_streams = options.max_concurrent_streams == 0
                                         ? std::max<std::size_t>(1, options.worker_threads - 1)
                                         : options.max_concurrent_streams;

    FileDescriptor instance_lock = acquire_instance_lock(state_->socket_path);
    remove_stale_socket(state_->socket_path, options.stale_socket_grace_period);

    FileDescriptor listener = make_socket();
    const sockaddr_un address = make_address(state_->socket_path);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), address_length(address)) != 0) {
        throw_system_error("bind failed");
    }

    struct stat identity {};
    if (::lstat(state_->socket_path.c_str(), &identity) != 0) {
        const int error = errno;
        (void)::unlink(state_->socket_path.c_str());
        throw_system_error("lstat bound socket failed", error);
    }
    if (!S_ISSOCK(identity.st_mode) || identity.st_uid != ::geteuid()) {
        (void)::unlink(state_->socket_path.c_str());
        throw std::runtime_error("bound socket path was replaced before initialization completed");
    }

    if (::chmod(state_->socket_path.c_str(), static_cast<mode_t>(options.socket_permissions)) != 0) {
        const int error = errno;
        (void)::unlink(state_->socket_path.c_str());
        throw_system_error("chmod socket path failed", error);
    }

    if (::listen(listener.get(), options.listen_backlog) != 0) {
        const int error = errno;
        (void)::unlink(state_->socket_path.c_str());
        throw_system_error("listen failed", error);
    }

    std::array<FileDescriptor, 2> wake_pipe = make_wakeup_pipe();

    std::lock_guard<std::mutex> lock(state_->lifecycle_mutex);
    state_->listener_fd = listener.release();
    state_->wake_read_fd = wake_pipe[0].release();
    state_->wake_write_fd = wake_pipe[1].release();
    state_->instance_lock_fd = instance_lock.release();
}

Server::~Server() {
    const auto state = state_;
    stop_state(state);

    std::unique_lock<std::mutex> lock(state->lifecycle_mutex);
    state->lifecycle_cv.wait(lock, [&state] { return !state->run_active; });
}

void Server::on(std::string route, Handler handler) {
    validate_route(route, state_->options.max_message_size);
    if (!handler) {
        throw std::invalid_argument("handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    if (!state_->handlers.emplace(std::move(route), detail::HandlerEntry{std::move(handler), false}).second) {
        throw std::runtime_error("route already exists");
    }
}

void Server::on_prefix(std::string prefix, Handler handler) {
    validate_route(prefix, state_->options.max_message_size);
    if (!handler) {
        throw std::invalid_argument("handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    for (const auto& entry : state_->handler_prefixes) {
        if (entry.first == prefix) {
            throw std::runtime_error("prefix route already exists");
        }
    }
    state_->handler_prefixes.emplace_back(std::move(prefix), detail::HandlerEntry{std::move(handler), false});
}

void Server::on_serialized(std::string route, Handler handler) {
    validate_route(route, state_->options.max_message_size);
    if (!handler) {
        throw std::invalid_argument("handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    if (!state_->handlers.emplace(std::move(route), detail::HandlerEntry{std::move(handler), true}).second) {
        throw std::runtime_error("route already exists");
    }
}

void Server::on_stream(std::string route, StreamHandler handler) {
    validate_route(route, state_->options.max_message_size);
    if (!handler) {
        throw std::invalid_argument("stream handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    if (!state_->stream_handlers.emplace(std::move(route), detail::StreamHandlerEntry{std::move(handler)}).second) {
        throw std::runtime_error("stream route already exists");
    }
}

void Server::on_stream_prefix(std::string prefix, StreamHandler handler) {
    validate_route(prefix, state_->options.max_message_size);
    if (!handler) {
        throw std::invalid_argument("stream handler must not be empty");
    }
    std::lock_guard<std::mutex> lock(state_->handlers_mutex);
    for (const auto& entry : state_->stream_prefixes) {
        if (entry.first == prefix) {
            throw std::runtime_error("prefix route already exists");
        }
    }
    state_->stream_prefixes.emplace_back(std::move(prefix), detail::StreamHandlerEntry{std::move(handler)});
}

void Server::enqueue_maintenance(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("maintenance task must not be empty");
    }
    const auto state = state_;
    if (!detail::ensure_serialized_worker(state)) {
        throw std::logic_error("server is not running");
    }
    std::unique_lock<std::mutex> lock(state->serialized_mutex);
    if (state->serialized_stopping.load() || !state->running.load()) {
        throw std::logic_error("server is not running");
    }
    detail::SerializedJob job;
    job.maintenance = std::move(task);
    state->pending_serialized.push_back(std::move(job));
    lock.unlock();
    state->serialized_cv.notify_one();
}

void Server::run() {
    const auto state = state_;

    int listener = -1;
    int wake_read = -1;
    {
        std::lock_guard<std::mutex> lock(state->lifecycle_mutex);
        if (state->run_started) {
            throw std::logic_error("server run() can only be called once");
        }
        state->run_started = true;
        if (state->stopped || state->listener_fd < 0 || state->wake_read_fd < 0) {
            throw std::logic_error("server has already been stopped");
        }
        state->run_active = true;
        state->running.store(true);
        listener = state->listener_fd;
        wake_read = state->wake_read_fd;
    }

    RunActiveGuard active_guard(state);

    // Create the epoll instance and register the listener + wakeup pipe.
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        throw_system_error("epoll_create1 failed");
    }
    state->epoll_fd = epoll_fd;

    epoll_event listener_event{};
    listener_event.events = EPOLLIN;
    listener_event.data.u64 = std::numeric_limits<std::uint64_t>::max();
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener, &listener_event) != 0) {
        throw_system_error("epoll_ctl(listener) failed");
    }
    epoll_event wake_event{};
    wake_event.events = EPOLLIN;
    wake_event.data.u64 = std::numeric_limits<std::uint64_t>::max() - 1;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_read, &wake_event) != 0) {
        throw_system_error("epoll_ctl(wakeup) failed");
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
        detail::join_serialized_worker(state);
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
    detail::join_serialized_worker(state);

    // Close every remaining connection now that no worker touches fds.
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        for (auto& [fd, rc] : state->connections) {
            (void)::shutdown(fd, SHUT_RDWR);
            (void)::close(fd);
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

} // namespace easy_uds
