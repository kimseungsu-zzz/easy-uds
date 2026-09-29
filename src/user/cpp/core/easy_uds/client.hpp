#pragma once

#if !defined(_WIN32)
#include "easy_uds/fd.hpp"
#else
#include "easy_uds/windows.hpp"
#endif
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

    // Reconnects and retries only transport failures. Use this only when the
    // route is idempotent: a timeout can happen after the server ran it but
    // before the response reached this client. max_attempts includes the first
    // attempt. Application status responses are returned without retrying.
    [[nodiscard]] Response request_idempotent(
        std::string_view route, std::string_view body = {},
        RetryOptions retry = {}) const;

#if !defined(_WIN32)
    // One-shot POSIX request that also passes a borrowed descriptor (a
    // duplicate is sent via SCM_RIGHTS; the caller keeps ownership). The
    // server exposes the received descriptor through the POSIX capability view
    // during a contextual handler callback. The response is read normally.
    [[nodiscard]] Response request_fd(std::string_view route, BorrowedFd fd,
                                      std::string_view body = {}) const;
#endif

#if defined(_WIN32)
    // One-shot Windows request that duplicates a caller-owned HANDLE into the
    // receiving server process. The caller keeps ownership; the source handle
    // must remain open until the response arrives.
    [[nodiscard]] Response request_handle(
        std::string_view route, windows::BorrowedHandle handle,
        std::string_view body = {}) const;
#endif

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
