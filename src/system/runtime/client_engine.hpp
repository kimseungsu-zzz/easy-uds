#pragma once

#include "easy_uds/client.hpp"

namespace easy_uds::detail::client_engine {

// Concrete one-shot engine operations used by the public Client glue. These
// functions intentionally keep the existing value types and call graph; they
// are not a polymorphic backend.
void validate(const std::string& socket_path, const ClientOptions& options);

Response request(const std::string& socket_path, const ClientOptions& options,
                 std::string_view route, std::string_view body);

#if !defined(_WIN32)
Response request_fd(const std::string& socket_path, const ClientOptions& options,
                    std::string_view route, BorrowedFd fd, std::string_view body);
#endif

Status request_stream(
    const std::string& socket_path, const ClientOptions& options,
    std::string_view route, const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk);

} // namespace easy_uds::detail::client_engine
