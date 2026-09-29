#include "easy_uds/client.hpp"
#include "easy_uds/error.hpp"

#include "../../../system/runtime/client_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace easy_uds {

Client::Client(std::string socket_path, ClientOptions options)
    : socket_path_(std::move(socket_path)), options_(options) {
    detail::client_engine::validate(socket_path_, options_);
}

Response Client::request(std::string_view route, std::string_view body) const {
    return detail::client_engine::request(socket_path_, options_, route, body);
}

Response Client::request_idempotent(std::string_view route,
                                    std::string_view body,
                                    RetryOptions retry) const {
    if (retry.max_attempts == 0 || retry.initial_backoff.count() < 0 ||
        retry.max_backoff.count() < 0 ||
        retry.initial_backoff > retry.max_backoff) {
        throw std::invalid_argument("invalid idempotent request retry options");
    }

    auto backoff = retry.initial_backoff;
    for (std::size_t attempt = 1;; ++attempt) {
        try {
            return request(route, body);
        } catch (const Error& error) {
            const bool retryable = error.kind() == ErrorCode::timeout ||
                                   error.kind() == ErrorCode::closed ||
                                   error.kind() == ErrorCode::unavailable;
            if (!retryable || attempt >= retry.max_attempts) {
                throw;
            }
            if (backoff.count() > 0) {
                std::this_thread::sleep_for(backoff);
            }
            if (backoff < retry.max_backoff) {
                const auto remaining = retry.max_backoff - backoff;
                backoff += std::min(backoff, remaining);
            }
        }
    }
}

#if !defined(_WIN32)
Response Client::request_fd(std::string_view route, BorrowedFd fd,
                            std::string_view body) const {
    return detail::client_engine::request_fd(socket_path_, options_, route, fd, body);
}
#endif

#if defined(_WIN32)
Response Client::request_handle(std::string_view route,
                                windows::BorrowedHandle handle,
                                std::string_view body) const {
    return detail::client_engine::request_handle(
        socket_path_, options_, route, handle.get(), body);
}
#endif

Status Client::request_stream(
    std::string_view route, const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk) const {
    return detail::client_engine::request_stream(socket_path_, options_, route, request_body,
                                                 response_chunk);
}

Session Client::session() const {
    return Session(socket_path_, options_);
}

} // namespace easy_uds
