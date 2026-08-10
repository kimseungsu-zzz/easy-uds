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
inline constexpr std::size_t default_stream_chunk_size = 64U * 1024U;
inline constexpr std::size_t default_max_stream_size = 1024U * 1024U * 1024U;

// A pull-based byte source. Implementations fill at most `capacity` bytes and
// return the number produced. Returning zero marks end-of-stream.
using StreamReader = std::function<std::size_t(char* buffer, std::size_t capacity)>;

// A streamed response is consumed immediately after the handler returns. Its
// reader must own (or otherwise outlive) every resource it captures.
struct StreamResponse {
    int status_code = 200;
    StreamReader body;
};

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

    // Buffer and wire-frame size used for streamed request/response bodies.
    // Stream data is never accumulated into a complete in-memory message.
    std::size_t stream_chunk_size = default_stream_chunk_size;

    // Maximum bytes in each streamed request body and response body. Zero
    // allows an unbounded stream; io_timeout still detects stalled peers.
    std::size_t max_stream_size = default_max_stream_size;

    // Maximum idle time between successful socket-I/O progress events.
    // Zero disables the inactivity timeout.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for a connection, measured from accept() until the
    // response is written. It prevents slow-drip peers from holding a worker
    // forever. Zero disables the absolute deadline. User handler execution is
    // not forcibly interrupted; if it runs past the deadline, response I/O
    // fails immediately when the handler returns.
    std::chrono::milliseconds request_timeout{30000};

    // Absolute deadline for a streaming exchange after its stream header has
    // arrived. Zero allows a long-lived stream, bounded only by io_timeout.
    std::chrono::milliseconds stream_timeout{0};

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

    std::size_t stream_chunk_size = default_stream_chunk_size;

    // Zero allows an unbounded stream.
    std::size_t max_stream_size = default_max_stream_size;

    // Maximum time spent establishing a connection. Zero disables this
    // connect-specific limit; request_timeout may still bound the operation.
    std::chrono::milliseconds connect_timeout{2000};

    // Maximum idle time between successful socket-I/O progress events.
    // Zero disables the inactivity timeout.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for connect + request write + response read.
    // Zero disables the absolute request deadline.
    std::chrono::milliseconds request_timeout{30000};

    // Absolute deadline for connect + streamed request + streamed response.
    // Zero allows a long-lived stream, bounded by connect_timeout/io_timeout.
    std::chrono::milliseconds stream_timeout{0};
};

namespace detail {
struct ServerState;
}

class Server {
  public:
    using Handler = std::function<Response(const Request&)>;
    using StreamHandler = std::function<StreamResponse(const StreamReader& request_body)>;

    explicit Server(std::string socket_path, ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Registers or rejects a duplicate route. May be called while run() is active.
    void on(std::string route, Handler handler);

    // Registers a streaming route. The handler pulls the request incrementally
    // and returns a pull-based response, so neither body must be held in memory.
    // A stream reader is valid only for the duration of its callback.
    void on_stream(std::string route, StreamHandler handler);

    // Overrides the automatic concurrent-stream limit, which reserves one
    // worker for regular RPCs when possible. The valid range is 1 through
    // worker_threads; zero is invalid. Must be called before run().
    void set_max_concurrent_streams(std::size_t limit);

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

    // Streams one request and response over a single connection. `request_body`
    // is pulled into fixed-size buffers. `response_chunk` is called with views
    // valid only until that callback returns. Returns the response status code.
    [[nodiscard]] int request_stream(std::string_view route, const StreamReader& request_body,
                                     const std::function<void(std::string_view)>& response_chunk) const;

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

  private:
    std::string socket_path_;
    ClientOptions options_;
};

} // namespace easy_uds
