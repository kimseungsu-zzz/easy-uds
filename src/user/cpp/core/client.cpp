#include "easy_uds/client.hpp"

#include "../../../system/runtime/client_engine.hpp"

#include <utility>

namespace easy_uds {

Client::Client(std::string socket_path, ClientOptions options)
    : socket_path_(std::move(socket_path)), options_(options) {
    detail::client_engine::validate(socket_path_, options_);
}

Response Client::request(std::string_view route, std::string_view body) const {
    return detail::client_engine::request(socket_path_, options_, route, body);
}

#if !defined(_WIN32)
Response Client::request_fd(std::string_view route, BorrowedFd fd,
                            std::string_view body) const {
    return detail::client_engine::request_fd(socket_path_, options_, route, fd, body);
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
