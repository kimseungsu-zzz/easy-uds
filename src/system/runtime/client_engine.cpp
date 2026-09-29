#include "client_engine.hpp"

#include "../platform/peer_identity.hpp"
#include "../transport/io.hpp"
#include "../transport/transport.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
namespace easy_uds::detail::client_engine {
namespace {

using namespace detail;
using protocol::HeaderBytes;
using protocol::WireType;

#if !defined(_WIN32)
void write_request_frame_with_fd(NativeSocket fd, std::uint32_t request_id,
                                 NativeSocket passed_fd,
                                 std::string_view route, std::string_view body,
                                 std::chrono::milliseconds io_timeout, Deadline deadline,
                                 std::uint8_t wire_version) {
    const HeaderBytes header = protocol::encode_header(
        WireType::request, request_id, static_cast<std::uint32_t>(route.size()),
        static_cast<std::uint32_t>(body.size()), protocol::carries_fd_flag,
        wire_version);
    std::array<iovec, 3> parts{{
        {const_cast<unsigned char*>(header.data()), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact_with_fd(fd, parts.data(), parts.size(), passed_fd, io_timeout,
                               deadline);
}
#endif

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
    detail::client::write_request_frame(fd.get(), 0, route, body,
                                        options.io_timeout, deadline,
                                        options.protocol_version);
    BufferedReader reader(fd.get());
    return read_response(reader, options.max_message_size, options.io_timeout, deadline);
}

#if defined(_WIN32)
Response request_handle(const std::string& socket_path,
                        const ClientOptions& options, std::string_view route,
                        HANDLE handle, std::string_view body) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        throw std::invalid_argument("request_handle requires a valid HANDLE");
    }
    const auto native_handle = reinterpret_cast<std::uintptr_t>(handle);
    if (native_handle >=
        std::numeric_limits<std::uintptr_t>::max() - 15U) {
        throw std::invalid_argument(
            "request_handle does not accept Windows pseudo handles");
    }
    constexpr std::size_t handle_prefix_size = sizeof(std::uint64_t);
    if (body.size() > std::numeric_limits<std::size_t>::max() -
                          handle_prefix_size) {
        throw std::length_error("request exceeds max_message_size");
    }
    protocol::validate_request_lengths(
        route.size(), body.size() + handle_prefix_size,
        options.max_message_size);
    const Deadline deadline = detail::deadline_from_now(options.request_timeout);
    FileDescriptor fd = detail::make_socket();
    const auto address = detail::make_address(socket_path);
    detail::connect_nonblocking(fd.get(), address, options.connect_timeout,
                                deadline);

    std::uint32_t server_pid = 0;
    if (!peer_identity::query_peer_process_id(fd.get(), server_pid)) {
        throw Error(ErrorCode::unavailable,
                    "could not identify the Windows AF_UNIX server process");
    }

    const auto raw_handle = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(handle));
    std::array<unsigned char, sizeof(raw_handle)> metadata{};
    for (std::size_t index = 0; index < metadata.size(); ++index) {
        metadata[index] = static_cast<unsigned char>(
            raw_handle >> ((metadata.size() - index - 1) * 8));
    }
    const std::size_t wire_body_size = body.size() + metadata.size();
    const HeaderBytes header = protocol::encode_header(
        WireType::request, 0, static_cast<std::uint32_t>(route.size()),
        static_cast<std::uint32_t>(wire_body_size),
        protocol::carries_windows_handle_flag, options.protocol_version);
    std::array<iovec, 4> parts{{
        {const_cast<unsigned char*>(header.data()), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {metadata.data(), metadata.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact(fd.get(), parts.data(), parts.size(), options.io_timeout,
                       deadline);
    BufferedReader reader(fd.get());
    return read_response(reader, options.max_message_size, options.io_timeout,
                         deadline);
}
#endif

#if !defined(_WIN32)
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
                                options.io_timeout, deadline,
                                options.protocol_version);
    BufferedReader reader(socket_fd.get());
    return read_response(reader, options.max_message_size, options.io_timeout, deadline);
}
#endif

Status request_stream(
    const std::string& socket_path, const ClientOptions& options,
    std::string_view route, const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk) {
    return detail::client::run_oneshot_stream(socket_path, options, route, request_body,
                                              response_chunk);
}

} // namespace easy_uds::detail::client_engine
