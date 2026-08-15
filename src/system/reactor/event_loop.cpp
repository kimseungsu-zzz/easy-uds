#include "core.hpp"

#include "common.hpp"
#include "parser.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace easy_uds::detail {
namespace {

void process_resumed_connections(const std::shared_ptr<ServerState>& state) {
    constexpr std::size_t max_batch = 64;
    for (std::size_t index = 0; index < max_batch; ++index) {
        std::shared_ptr<ReactorConnection> connection;
        {
            std::lock_guard<std::mutex> lock(state->connections_mutex);
            if (state->resumed_connections.empty()) {
                return;
            }
            connection = std::move(state->resumed_connections.front());
            state->resumed_connections.pop_front();
            const auto it = state->connections.find(connection->conn->fd);
            if (it == state->connections.end() || it->second != connection ||
                connection->read_paused || connection->reactor_busy ||
                connection->conn->closing.load(std::memory_order_acquire) ||
                connection->conn->stream_active.load(std::memory_order_acquire) ||
                connection->conn->worker_owned.load(std::memory_order_acquire)) {
                continue;
            }
            connection->reactor_busy = true;
        }

        try {
            consume(state, connection);
        } catch (...) {
            connection->conn->closing.store(true, std::memory_order_release);
        }
        if (connection->conn->closing.load(std::memory_order_acquire)) {
            close_connection(state, connection->conn->fd);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(state->connections_mutex);
            const auto it = state->connections.find(connection->conn->fd);
            if (it != state->connections.end() && it->second == connection) {
                connection->reactor_busy = false;
            }
        }
    }
}

Deadline expire_reactor_connections(const std::shared_ptr<ServerState>& state) {
    const Deadline now = Clock::now();
    Deadline next = Deadline::max();
    std::vector<int> expired;
    {
        std::lock_guard<std::mutex> lock(state->connections_mutex);
        expired.reserve(state->connections.size());
        for (const auto& [fd, reactor_connection] : state->connections) {
            const auto& connection = reactor_connection->conn;
            if (connection->stream_active.load(std::memory_order_acquire) ||
                connection->worker_owned.load(std::memory_order_acquire)) {
                continue;
            }
            if (connection->closing.load(std::memory_order_acquire)) {
                expired.push_back(fd);
                continue;
            }

            Deadline connection_deadline = reactor_connection->deadline;
            connection_deadline = earlier_deadline(
                connection_deadline, connection_output_deadline(state, connection));
            const bool partial_request =
                reactor_connection->phase != ParsePhase::header ||
                reactor_connection->pending_offset != reactor_connection->pending.size();
            const bool response_pending =
                connection->active_regular.load(std::memory_order_acquire) != 0 ||
                connection->pending_serialized.load(std::memory_order_acquire) != 0;
            if (partial_request || !response_pending) {
                connection_deadline = earlier_deadline(
                    connection_deadline,
                    connection_inactivity_deadline(connection,
                                                   state->options.io_timeout));
            }
            if (connection_deadline != Deadline::max() && now >= connection_deadline) {
                expired.push_back(fd);
            } else {
                next = earlier_deadline(next, connection_deadline);
            }
        }
    }
    for (const int fd : expired) {
        close_connection(state, fd);
    }
    return next;
}

} // namespace

void run_reactor(const std::shared_ptr<ServerState>& state) {
    const int listener = state->listener_fd;
    const int wake_read = state->wake_read_fd;
    const int epoll_fd = state->epoll_fd;

    std::array<epoll_event, 128> events{};
    while (state->running.load()) {
        process_resumed_connections(state);
        const Deadline next_deadline = expire_reactor_connections(state);
        const int count = ::epoll_wait(epoll_fd, events.data(),
                                       static_cast<int>(events.size()),
                                       poll_timeout_ms(next_deadline));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!state->running.load()) {
                break;
            }
            throw_system_error("epoll_wait failed");
        }
        if (count == 0) {
            continue;
        }

        for (int index = 0; index < count; ++index) {
            const std::uint64_t token = events[index].data.u64;
            const std::uint32_t mask = events[index].events;

            if (token == listener_token) {
                if ((mask & (EPOLLERR | EPOLLHUP)) != 0) {
                    throw std::runtime_error("listening socket failed");
                }
                std::size_t accepted = 0;
                while (accepted < max_accept_batch) {
                    const int client_fd = platform_linux::accept_socket(listener);
                    if (client_fd < 0) {
                        if (errno == EINTR || errno == ECONNABORTED) {
                            continue;
                        }
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        throw_system_error("accept failed");
                    }
                    ++accepted;
                    {
                        std::lock_guard<std::mutex> lock(state->connections_mutex);
                        if (state->connections.size() >=
                            state->options.max_connections) {
                            record_rejected_connection(state);
                            (void)::close(client_fd);
                            continue;
                        }
                        auto connection = std::make_shared<Connection>(
                            client_fd, capture_peer_credentials(client_fd));
                        auto reactor_connection =
                            std::make_shared<ReactorConnection>();
                        reactor_connection->conn = connection;
                        reactor_connection->generation =
                            allocate_connection_generation(state);
                        reactor_connection->registered_events = EPOLLIN;
                        state->connections[client_fd] = reactor_connection;
                        epoll_event event{};
                        event.events = reactor_connection->registered_events;
                        event.data.u64 = connection_token(
                            client_fd, reactor_connection->generation);
                        if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd,
                                        &event) != 0) {
                            state->connections.erase(client_fd);
                        } else {
                            record_accepted_connection(state);
                        }
                    }
                }
                continue;
            }

            if (token == wake_token) {
                // Clear before draining: a concurrent producer that observes
                // false will enqueue another eventfd count, while producers
                // that observed true are covered by the count being drained.
                state->wake_pending.store(false, std::memory_order_release);
                std::uint64_t counter = 0;
                while (::read(wake_read, &counter, sizeof(counter)) > 0) {
                }
                index = count;
                break;
            }

            const int fd = static_cast<int>(static_cast<std::uint32_t>(token));
            const std::uint32_t generation =
                static_cast<std::uint32_t>(token >> 32);
            std::shared_ptr<ReactorConnection> connection;
            bool close_requested = false;
            bool read_allowed = false;
            {
                std::lock_guard<std::mutex> lock(state->connections_mutex);
                const auto it = state->connections.find(fd);
                if (it == state->connections.end() ||
                    it->second->generation != generation) {
                    continue;
                }
                connection = it->second;
                if (connection->conn->closing.load(std::memory_order_acquire)) {
                    close_requested = true;
                } else if (
                    connection->conn->stream_active.load(std::memory_order_acquire) ||
                    connection->conn->worker_owned.load(std::memory_order_acquire)) {
                    continue;
                } else {
                    connection->reactor_busy = true;
                    read_allowed = !connection->read_paused;
                }
            }
            if (close_requested) {
                close_connection(state, fd);
                continue;
            }
            if ((mask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                close_connection(state, fd);
                continue;
            }
            if ((mask & EPOLLOUT) != 0 &&
                !flush_connection_output(state, connection)) {
                close_connection(state, fd);
                continue;
            }
            if ((mask & EPOLLIN) != 0 && read_allowed) {
                try {
                    consume(state, connection);
                } catch (...) {
                    connection->conn->closing.store(true,
                                                    std::memory_order_release);
                }
                if (connection->conn->closing.load(std::memory_order_acquire)) {
                    close_connection(state, fd);
                    continue;
                }
            }
            {
                std::lock_guard<std::mutex> lock(state->connections_mutex);
                const auto it = state->connections.find(fd);
                if (it != state->connections.end() && it->second == connection) {
                    connection->reactor_busy = false;
                }
            }
        }
    }
}

} // namespace easy_uds::detail
