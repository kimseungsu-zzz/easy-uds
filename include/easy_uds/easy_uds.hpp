#pragma once

#include "easy_uds/error.hpp"
#include "easy_uds/fd.hpp"
#include "easy_uds/version.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <sys/types.h>

namespace easy_uds {

// ---- Response status codes -------------------------------------------------
// Wire-transparent 32-bit status; the well-known set below mirrors the
// constants the server generates. Handlers may return any non-negative value.
using Status = std::int32_t;

inline constexpr Status status_ok = 200;
inline constexpr Status status_created = 201;
inline constexpr Status status_bad_request = 400;
inline constexpr Status status_request_timeout = 408;
inline constexpr Status status_not_found = 404;
inline constexpr Status status_conflict = 409;
inline constexpr Status status_internal_error = 500;
inline constexpr Status status_unavailable = 503;

// ---- Sizes ----------------------------------------------------------------
inline constexpr std::size_t default_max_message_size = 1024U * 1024U;
inline constexpr std::size_t default_stream_chunk_size = 64U * 1024U;
inline constexpr std::size_t default_max_stream_size = 1024U * 1024U * 1024U;
inline constexpr std::size_t default_max_inflight_requests_per_connection = 64;
inline constexpr std::size_t default_max_inflight_request_bytes_per_connection = 4U * 1024U * 1024U;
inline constexpr std::size_t default_max_output_bytes_per_connection = 4U * 1024U * 1024U;

// ---- Request / response ---------------------------------------------------
// Peer identity of the connecting client, captured with SO_PEERCRED on Linux.
// `present` is false when the socket cannot provide credentials.
struct PeerCredentials {
    pid_t pid = -1;
    uid_t uid = static_cast<uid_t>(-1);
    gid_t gid = static_cast<gid_t>(-1);
    bool present = false;
};

struct Request {
    std::string route;
    std::string body;
    PeerCredentials peer;
    // Correlation id for the multiplexed protocol. The client assigns it per
    // in-flight request; responses to different requests may arrive in any
    // order. Handlers that mutate shared state must not rely on it being
    // sequential.
    std::uint32_t request_id = 0;
    // Descriptor passed with the request via `request_fd()`, or an empty value
    // when the request carried none. Request owns the received descriptor and
    // closes it automatically. A handler that wants to retain access beyond
    // its return must store `fd.duplicate()`.
    OwnedFd fd;
};

struct Response {
    Status status = status_ok;
    std::string body;
};

// A pull-based byte source. Implementations fill at most `capacity` bytes and
// return the number produced. Returning zero marks end-of-stream.
using StreamReader = std::function<std::size_t(char* buffer, std::size_t capacity)>;

// A streamed response is consumed immediately after the handler returns. Its
// reader must own (or otherwise outlive) every resource it captures.
struct StreamResponse {
    Status status = status_ok;
    StreamReader body;
};

// ---- Options --------------------------------------------------------------
struct ServerOptions {
    // Fixed worker-pool size used to execute handlers. Must be > 0.
    std::size_t worker_threads = 4;

    // Maximum number of concurrently open client connections.
    std::size_t max_connections = 64;

    // Maximum route+body request bytes and maximum response-body bytes.
    std::size_t max_message_size = default_max_message_size;

    // Buffer and wire-frame size used for streamed request/response bodies.
    std::size_t stream_chunk_size = default_stream_chunk_size;

    // Maximum bytes in each streamed request body and response body. Zero
    // allows an unbounded stream; io_timeout still detects stalled peers.
    std::size_t max_stream_size = default_max_stream_size;

    // Strict aggregate declared route+body budget across all connections. A
    // frame reserves its bytes after header validation and before parser
    // buffers are allocated, so partial, queued, and executing requests are
    // included. Zero disables the aggregate limit and preserves the session
    // continuation fast path.
    std::size_t max_total_inflight_bytes = 0;

    // Aggregate unsent fixed-response wire-byte budget (header + remaining
    // body) across all connections. Zero disables the global limit;
    // per-connection output limits remain.
    std::size_t max_total_output_bytes = 0;

    // Maximum number of queued or executing fixed requests per connection.
    // This is the primary per-peer memory/backpressure guard.
    std::size_t max_inflight_requests_per_connection = default_max_inflight_requests_per_connection;

    // Maximum aggregate route+body bytes retained for queued or executing
    // fixed requests on one connection. Must fit at least one max_message.
    std::size_t max_inflight_request_bytes_per_connection =
        default_max_inflight_request_bytes_per_connection;

    // Maximum unsent fixed-response wire bytes for one connection. A slow peer
    // exceeding this limit is closed; the default preserves the 4 MiB cap.
    std::size_t max_output_bytes_per_connection = default_max_output_bytes_per_connection;

    // Maximum simultaneous streams. Zero means automatic: reserve one worker
    // for regular RPC (`worker_threads - 1`, at least 1). Explicit values must
    // be between 1 and worker_threads.
    std::size_t max_concurrent_streams = 0;

    // Maximum idle time between successful socket-I/O progress events.
    // Zero disables the inactivity timeout.
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for a request, measured from its first header byte
    // until its response is written. Zero disables it. Handler execution
    // is not forcibly interrupted; if it runs past the deadline, response I/O
    // fails immediately when the handler returns.
    std::chrono::milliseconds request_timeout{30000};

    // Absolute deadline for a streaming exchange after its stream header has
    // arrived. Zero allows a long-lived stream, bounded only by io_timeout.
    std::chrono::milliseconds stream_timeout{0};

    // A worker that just served a fixed request waits directly for one next
    // request during this grace period (avoiding a reactor dispatch hop). It
    // returns the connection to the reactor before executing that request, so
    // later multiplexed requests can still run concurrently. `0` disables the
    // continuation fast path.
    std::chrono::milliseconds session_idle_grace{1};

    // When a socket pathname exists but refuses connections, wait this long
    // before considering it stale. Zero performs no grace wait.
    std::chrono::milliseconds stale_socket_grace_period{250};

    // Backlog passed to listen().
    int listen_backlog = 64;

    // Filesystem mode applied to the Unix socket pathname.
    unsigned int socket_permissions = 0600;

    // Include handler exception messages (and response-rejection reasons) in
    // 500 response bodies so clients can see the root cause. Disable when
    // clients must not learn internal error details.
    bool include_handler_error_messages = true;
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
    std::chrono::milliseconds io_timeout{5000};

    // Absolute deadline for connect + request write + response read.
    std::chrono::milliseconds request_timeout{30000};

    // Absolute deadline for connect + streamed request + streamed response.
    std::chrono::milliseconds stream_timeout{0};
};

// ---- Server ---------------------------------------------------------------
namespace detail {
struct ServerState;
struct SessionState;
}

// A request/response server over a Unix Domain Socket. Internally an epoll
// reactor accepts connections, parses frames, and drains bounded output queues;
// a fixed worker pool executes handlers. Long-lived connections and peers that
// stop reading never occupy a worker while idle or blocked on output.
class Server {
  public:
    using Handler = std::function<Response(const Request&)>;
    using StreamHandler = std::function<StreamResponse(const StreamReader&, const Request&)>;

    explicit Server(std::string socket_path, ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // Exact-match route. May be called while run() is active.
    void on(std::string route, Handler handler);

    // Prefix route: matches every route that starts with `prefix` and has no
    // more specific exact match. Shorter prefixes lose to longer ones. May be
    // called while run() is active.
    void on_prefix(std::string prefix, Handler handler);

    // Exclusive FIFO executor shared by every serialized route. Waiting
    // serialized requests do not occupy the normal worker pool.
    void on_serialized(std::string route, Handler handler);

    // Streaming route. Exact-match variant.
    void on_stream(std::string route, StreamHandler handler);

    // Streaming route. Prefix-match variant.
    void on_stream_prefix(std::string prefix, StreamHandler handler);

    // Runs `task` on the same FIFO executor as serialized handlers, strictly
    // ordered against them. Throws std::logic_error when the server is not
    // running.
    void enqueue_maintenance(std::function<void()> task);

    // Starts the reactor and worker pool and blocks until stop(). A Server is
    // single-use.
    void run();

    // Idempotent. Safe to call from another thread.
    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] const std::string& socket_path() const noexcept;

  private:
    std::shared_ptr<detail::ServerState> state_;
};

// ---- Client---------------------------------------------------------------
class Session;

class Client {
  public:
    explicit Client(std::string socket_path, ClientOptions options = {});

    // Opens one connection, sends one request, receives one response, then
    // closes. Multiple threads may call request() concurrently.
    [[nodiscard]] Response request(std::string_view route, std::string_view body = {}) const;

    // One-shot request that also passes a borrowed descriptor (a duplicate is
    // sent via SCM_RIGHTS; the caller keeps ownership). The server delivers an
    // owning `Request::fd` to the handler. The response is read as a normal
    // fixed response.
    [[nodiscard]] Response request_fd(std::string_view route, BorrowedFd fd,
                                      std::string_view body = {}) const;

    // One-shot streamed exchange over a dedicated connection.
    [[nodiscard]] Status request_stream(std::string_view route, const StreamReader& request_body,
                                        const std::function<void(std::string_view)>& response_chunk) const;

    // Opens a persistent, multiplexed connection. Concurrent request() calls
    // on the returned session are pipelined and correlated by request id.
    [[nodiscard]] Session session() const;

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

  private:
    std::string socket_path_;
    ClientOptions options_;
};

// A persistent connection opened by Client::session(). Concurrent request()
// calls are fully multiplexed: the server may answer out of order and each
// call returns its own response. request_stream() uses a separate dedicated
// connection, so it does not block fixed requests or another stream call.
// After an I/O error, time-out, or peer close the fixed-request session is
// permanently unusable and every later call throws Error with
// ErrorCode::closed. A moved-from Session still throws std::logic_error.
class Session {
  public:
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept;
    Session& operator=(Session&&) noexcept;

    [[nodiscard]] Response request(std::string_view route, std::string_view body = {});

    [[nodiscard]] Status request_stream(std::string_view route, const StreamReader& request_body,
                                        const std::function<void(std::string_view)>& response_chunk);

  private:
    friend class Client;
    Session(std::string socket_path, ClientOptions options);
    std::unique_ptr<detail::SessionState> state_;
};

} // namespace easy_uds
