#pragma once

#include "easy_uds/fd.hpp"
#include "easy_uds/options.hpp"
#include "easy_uds/response.hpp"
#include "easy_uds/session.hpp"
#include "easy_uds/stream.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace easy_uds {

class Client {
  public:
    explicit Client(std::string socket_path, ClientOptions options = {});

    // Opens one connection, sends one request, receives one response, then
    // closes. Multiple threads may call request() concurrently.
    [[nodiscard]] Response request(std::string_view route,
                                   std::string_view body = {}) const;

    // One-shot request that also passes a borrowed descriptor (a duplicate is
    // sent via SCM_RIGHTS; the caller keeps ownership). The server delivers an
    // owning `Request::fd` to the handler. The response is read as a normal
    // fixed response.
    [[nodiscard]] Response request_fd(std::string_view route, BorrowedFd fd,
                                      std::string_view body = {}) const;

    // One-shot streamed exchange over a dedicated connection.
    [[nodiscard]] Status request_stream(
        std::string_view route, const StreamReader& request_body,
        const std::function<void(std::string_view)>& response_chunk) const;

    // Opens a persistent, multiplexed connection. Concurrent request() calls
    // on the returned session are pipelined and correlated by request id.
    [[nodiscard]] Session session() const;

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }

  private:
    std::string socket_path_;
    ClientOptions options_;
};

} // namespace easy_uds
