#pragma once

#include "easy_uds/options.hpp"
#include "easy_uds/request.hpp"
#include "easy_uds/request_context.hpp"
#include "easy_uds/response.hpp"
#include "easy_uds/stats.hpp"
#include "easy_uds/stream.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace easy_uds {

namespace detail {
struct ServerState;
}

// Advanced fixed-route registration. Keeping contextual handlers behind one
// options object leaves room for later scheduling and queue-policy controls
// without multiplying the beginner-facing Server methods.
class RouteOptions {
  public:
    using Handler =
        std::function<Response(const Request&, const RequestContext&)>;

    explicit RouteOptions(Handler handler) : handler_(std::move(handler)) {}

  private:
    friend class Server;
    Handler handler_;
};

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
    // Contextual form for handlers that need deadline/cancellation metadata.
    // Future advanced controls extend RouteOptions instead of adding handler
    // overloads. The RequestContext is valid only during the callback.
    void on(std::string route, RouteOptions options);

    // Prefix route: matches every route that starts with `prefix` and has no
    // more specific exact match. Shorter prefixes lose to longer ones. May be
    // called while run() is active.
    void on_prefix(std::string prefix, Handler handler);
    void on_prefix(std::string prefix, RouteOptions options);

    // Exclusive FIFO executor shared by every serialized route. Waiting
    // serialized requests do not occupy the normal worker pool.
    void on_serialized(std::string route, Handler handler);
    void on_serialized(std::string route, RouteOptions options);

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
    // Thread-safe best-effort snapshot. It may briefly acquire reactor and
    // executor bookkeeping mutexes but never waits for handlers or socket I/O.
    [[nodiscard]] ServerStats stats() const;

  private:
    std::shared_ptr<detail::ServerState> state_;
};

} // namespace easy_uds
