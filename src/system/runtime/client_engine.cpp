#include "client_engine.hpp"

#include "../transport/io.hpp"
#include "../transport/transport.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace easy_uds::detail::client_engine {
namespace {

using namespace detail;
using protocol::HeaderBytes;
using protocol::WireType;

void write_request_frame_with_fd(int fd, std::uint32_t request_id, int passed_fd,
                                 std::string_view route, std::string_view body,
                                 std::chrono::milliseconds io_timeout, Deadline deadline) {
    const HeaderBytes header = protocol::encode_header(
        WireType::request, request_id, static_cast<std::uint32_t>(route.size()),
        static_cast<std::uint32_t>(body.size()), protocol::carries_fd_flag);
    std::array<iovec, 3> parts{{
        {const_cast<unsigned char*>(header.data()), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact_with_fd(fd, parts.data(), parts.size(), passed_fd, io_timeout,
                               deadline);
}

Response read_response(BufferedReader& reader, std::size_t max_message_size,
                       std::chrono::milliseconds io_timeout, Deadline deadline) {
    HeaderBytes header{};
    reader.read(header.data(), header.size(), io_timeout, deadline);
    const auto decoded = protocol::decode_header(header, WireType::response);
    if (decoded.request_id != 0) {
        throw Error(ErrorCode::protocol, "unexpected response request_id");
    }
    if (decoded.arg1 > static_cast<std::uint32_t>(INT32_MAX)) {
        throw Error(ErrorCode::protocol, "response status_code is out of range");
    }
    if (decoded.arg2 > max_message_size) {
        throw Error(ErrorCode::too_large, "response exceeds max_message_size");
    }
    Response response;
    response.status = static_cast<Status>(decoded.arg1);
    response.body.resize(decoded.arg2);
    reader.read(response.body.data(), response.body.size(), io_timeout, deadline);
    return response;
}

} // namespace

void validate(const std::string& socket_path, const ClientOptions& options) {
    (void)detail::make_address(socket_path);
    detail::validate_client_options(options);
}

Response request(const std::string& socket_path, const ClientOptions& options,
                 std::string_view route, std::string_view body) {
    detail::client::validate_request_lengths(route, body, options.max_message_size);
    const Deadline deadline = detail::deadline_from_now(options.request_timeout);
    FileDescriptor fd = detail::make_socket();
    const auto address = detail::make_address(socket_path);
    detail::connect_nonblocking(fd.get(), address, options.connect_timeout, deadline);
    detail::client::write_request_frame(fd.get(), 0, route, body, options.io_timeout, deadline);
    BufferedReader reader(fd.get());
    return read_response(reader, options.max_message_size, options.io_timeout, deadline);
}

Response request_fd(const std::string& socket_path, const ClientOptions& options,
                    std::string_view route, BorrowedFd fd, std::string_view body) {
    if (!fd.valid()) {
        throw std::invalid_argument("request_fd requires a valid descriptor");
    }
    detail::client::validate_request_lengths(route, body, options.max_message_size);
    const Deadline deadline = detail::deadline_from_now(options.request_timeout);
    FileDescriptor socket_fd = detail::make_socket();
    const auto address = detail::make_address(socket_path);
    detail::connect_nonblocking(socket_fd.get(), address, options.connect_timeout, deadline);
    write_request_frame_with_fd(socket_fd.get(), 0, fd.get(), route, body,
                                options.io_timeout, deadline);
    BufferedReader reader(socket_fd.get());
    return read_response(reader, options.max_message_size, options.io_timeout, deadline);
}

Status request_stream(
    const std::string& socket_path, const ClientOptions& options,
    std::string_view route, const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk) {
    return detail::client::run_oneshot_stream(socket_path, options, route, request_body,
                                              response_chunk);
}

} // namespace easy_uds::detail::client_engine
