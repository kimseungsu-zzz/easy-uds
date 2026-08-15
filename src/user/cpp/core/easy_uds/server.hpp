#pragma once

#include "easy_uds/options.hpp"
#include "easy_uds/request.hpp"
#include "easy_uds/request_context.hpp"
#include "easy_uds/response.hpp"
#include "easy_uds/stats.hpp"
#include "easy_uds/stream.hpp"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace easy_uds {

namespace detail {
struct ServerState;
}

enum class QueuePolicy {
    fifo = 0,
    latest_wins = 1,
    reject_if_busy = 2,
};

// Advanced fixed-route registration. Keeping contextual handlers behind one
// options object leaves room for later scheduling and queue-policy controls
// without multiplying the beginner-facing Server methods.
class RouteOptions {
  public:
    using SimpleHandler = std::function<Response(const Request&)>;
    using ContextHandler =
        std::function<Response(const Request&, const RequestContext&)>;
    // Compatibility alias from the initial RequestContext API.
    using Handler = ContextHandler;

    explicit RouteOptions(SimpleHandler handler)
        : simple_handler_(std::move(handler)) {}
    explicit RouteOptions(ContextHandler handler)
        : context_handler_(std::move(handler)) {}

    RouteOptions& serialize_in(
        std::string domain, QueuePolicy policy = QueuePolicy::fifo) & {
        set_serialization(std::move(domain), policy);
        return *this;
    }

    RouteOptions&& serialize_in(
        std::string domain, QueuePolicy policy = QueuePolicy::fifo) && {
        set_serialization(std::move(domain), policy);
        return std::move(*this);
    }

    [[nodiscard]] bool serialized() const noexcept { return serialized_; }
    [[nodiscard]] const std::string& serialization_domain() const noexcept {
        return serialization_domain_;
    }
    [[nodiscard]] QueuePolicy queue_policy() const noexcept {
        return queue_policy_;
    }

  private:
    friend class Server;

    void set_serialization(std::string domain, QueuePolicy policy) {
        if (policy != QueuePolicy::fifo &&
            policy != QueuePolicy::latest_wins &&
            policy != QueuePolicy::reject_if_busy) {
            throw std::invalid_argument("unknown queue policy");
        }
        serialized_ = true;
        serialization_domain_ = std::move(domain);
        queue_policy_ = policy;
    }

    SimpleHandler simple_handler_;
    ContextHandler context_handler_;
    std::string serialization_domain_;
    QueuePolicy queue_policy_ = QueuePolicy::fifo;
    bool serialized_ = false;
};

// A request/response server over a Unix Domain Socket. Internally an epoll
// reactor accepts connections, parses frames, and drains bounded output queues;
// a fixed worker pool executes handlers. Long-lived connections and peers that
// stop reading never occupy a worker while idle or blocked on output.
class Server {
  public:
    using Handler = RouteOptions::SimpleHandler;
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

    // Runs `task` in the default FIFO domain, strictly ordered with
    // on_serialized() handlers. Throws std::logic_error when the server is not
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
