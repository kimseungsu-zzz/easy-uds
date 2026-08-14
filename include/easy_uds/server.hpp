#pragma once

#include "easy_uds/options.hpp"
#include "easy_uds/request.hpp"
#include "easy_uds/response.hpp"
#include "easy_uds/stream.hpp"

#include <functional>
#include <memory>
#include <string>

namespace easy_uds {

namespace detail {
struct ServerState;
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

} // namespace easy_uds
