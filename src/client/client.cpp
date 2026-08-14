#include "easy_uds/client.hpp"

#include "../detail/io.hpp"
#include "transport.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace easy_uds {
namespace {

using namespace detail;
using protocol::HeaderBytes;
using protocol::WireType;

// The frame header marks the request as carrying a descriptor and the
// descriptor rides as SCM_RIGHTS on the first sendmsg.
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

Client::Client(std::string socket_path, ClientOptions options)
    : socket_path_(std::move(socket_path)), options_(options) {
    (void)detail::make_address(socket_path_);
    detail::validate_client_options(options_);
}

Response Client::request(std::string_view route, std::string_view body) const {
    detail::client::validate_request_lengths(route, body, options_.max_message_size);

    const detail::Deadline deadline = detail::deadline_from_now(options_.request_timeout);
    detail::FileDescriptor fd = detail::make_socket();
    const sockaddr_un address = detail::make_address(socket_path_);
    detail::connect_nonblocking(fd.get(), address, options_.connect_timeout, deadline);

    detail::client::write_request_frame(fd.get(), 0, route, body, options_.io_timeout,
                                        deadline);
    detail::BufferedReader reader(fd.get());
    return read_response(reader, options_.max_message_size, options_.io_timeout, deadline);
}

Response Client::request_fd(std::string_view route, BorrowedFd fd,
                            std::string_view body) const {
    if (!fd.valid()) {
        throw std::invalid_argument("request_fd requires a valid descriptor");
    }
    detail::client::validate_request_lengths(route, body, options_.max_message_size);

    const detail::Deadline deadline = detail::deadline_from_now(options_.request_timeout);
    detail::FileDescriptor socket_fd = detail::make_socket();
    const sockaddr_un address = detail::make_address(socket_path_);
    detail::connect_nonblocking(socket_fd.get(), address, options_.connect_timeout, deadline);

    write_request_frame_with_fd(socket_fd.get(), 0, fd.get(), route, body,
                                options_.io_timeout, deadline);
    detail::BufferedReader reader(socket_fd.get());
    return read_response(reader, options_.max_message_size, options_.io_timeout, deadline);
}

Status Client::request_stream(
    std::string_view route, const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk) const {
    return detail::client::run_oneshot_stream(socket_path_, options_, route, request_body,
                                              response_chunk);
}

Session Client::session() const {
    return Session(socket_path_, options_);
}

} // namespace easy_uds
