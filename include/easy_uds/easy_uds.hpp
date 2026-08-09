#pragma once

#include "easy_uds/version.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace easy_uds {

inline constexpr std::size_t default_max_message_size = 1024U * 1024U;

struct Request {
    std::string route;
    std::string body;
};

struct Response {
    int status_code = 200;
    std::string body;
};

struct ServerOptions {
    // Fixed worker-pool size. Must be > 0 and <= max_connections.
    std::size_t worker_threads = 4;

    // Maximum number of active plus queued client connections.
    std::size_t max_connections = 64;

    // Maximum route+body request bytes and maximum response-body bytes.
    std::size_t max_message_size = default_max_message_size;

    // Maximum idle time between successful socket-I/O progress events.
    // Zero disables the inactivity timeout.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for a connection, measured from accept() until the
    // response is written. It prevents slow-drip peers from holding a worker
    // forever. Zero disables the absolute deadline. User handler execution is
    // not forcibly interrupted; if it runs past the deadline, response I/O
    // fails immediately when the handler returns.
    std::chrono::milliseconds request_timeout{30000};

    // When a socket pathname exists but refuses connections, wait this long
    // before considering it stale. This reduces races with another process
    // that has bound() but not yet listen()ed. Zero performs no grace wait.
    std::chrono::milliseconds stale_socket_grace_period{250};

    // Backlog passed to listen().
    int listen_backlog = 64;

    // Filesystem mode applied to the Unix socket pathname.
    unsigned int socket_permissions = 0600;
};

struct ClientOptions {
    std::size_t max_message_size = default_max_message_size;

    // Maximum time spent establishing a connection. Zero disables this
    // connect-specific limit; request_timeout may still bound the operation.
    std::chrono::milliseconds connect_timeout{2000};

    // Maximum idle time between successful socket-I/O progress events.
    // Zero disables the inactivity timeout.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for connect + request write + response read.
    // Zero disables the absolute request deadline.
    std::chrono::milliseconds request_timeout{30000};
};

namespace detail {
struct ServerState;
}

class Server {
  public:
    using Handler = std::function<Response(const Request&)>;

    explicit Server(std::string socket_path, ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Registers or rejects a duplicate route. May be called while run() is active.
    void on(std::string route, Handler handler);

    // Starts the fixed worker pool and blocks in the accept loop. A Server is
    // single-use: run() cannot be started again after it returns or after stop().
    void run();

    // Idempotent. Safe to call from another thread; wakes the accept loop and
    // interrupts blocked client socket I/O without closing a descriptor that
    // the run thread may still be polling.
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] const std::string& socket_path() const noexcept;

  private:
    std::shared_ptr<detail::ServerState> state_;
};

class Client {
  public:
    explicit Client(std::string socket_path, ClientOptions options = {});

    // Opens one connection, sends one request, receives one response, then closes.
    // Multiple threads may call request() concurrently on the same Client object.
    [[nodiscard]] Response request(std::string_view route, std::string_view body = {}) const;

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

  private:
    std::string socket_path_;
    ClientOptions options_;
};

} // namespace easy_uds
