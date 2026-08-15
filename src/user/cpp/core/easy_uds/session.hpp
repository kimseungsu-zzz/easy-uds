#pragma once

#include "easy_uds/options.hpp"
#include "easy_uds/response.hpp"
#include "easy_uds/stats.hpp"
#include "easy_uds/stream.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace easy_uds {

namespace detail {
struct SessionState;
}

// A snapshot of the persistent fixed-request connection's observed state.
// `active` means that no failure has been observed yet; it is not a liveness
// probe and the peer may disconnect immediately after the snapshot.
enum class SessionStatus {
    active = 0,
    broken = 1,
    moved_from = 2,
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

    // Lock-free snapshots. Safe to call concurrently with request() as long as
    // this Session object is not concurrently moved or destroyed.
    [[nodiscard]] SessionStatus status() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    // Thread-safe snapshot of the multiplexed fixed-request Session. Throws
    // std::logic_error for a moved-from Session.
    [[nodiscard]] SessionStats stats() const;

    [[nodiscard]] Response request(std::string_view route, std::string_view body = {});

    [[nodiscard]] Status request_stream(
        std::string_view route, const StreamReader& request_body,
        const std::function<void(std::string_view)>& response_chunk);

  private:
    friend class Client;
    Session(std::string socket_path, ClientOptions options);
    std::unique_ptr<detail::SessionState> state_;
};

} // namespace easy_uds
