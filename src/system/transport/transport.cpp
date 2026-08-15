#include "transport.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace easy_uds::detail::client {

using protocol::HeaderBytes;
using protocol::WireType;

namespace {

void write_stream_request(int fd, std::uint32_t request_id, std::string_view route,
                          const StreamReader& body, std::size_t chunk_size,
                          std::size_t max_stream_size,
                          std::chrono::milliseconds io_timeout, Deadline deadline) {
    write_frame_with_payload(fd, WireType::stream_request, request_id,
                             static_cast<std::uint32_t>(route.size()), 0, route.data(),
                             route.size(), io_timeout, deadline);

    std::vector<char> buffer(chunk_size);
    std::size_t total_size = 0;
    while (body) {
        const std::size_t size = body(buffer.data(), buffer.size());
        if (size == 0) {
            break;
        }
        if (size > buffer.size()) {
            throw std::runtime_error("stream reader returned more bytes than its capacity");
        }
        if (size > std::numeric_limits<std::size_t>::max() - total_size ||
            (max_stream_size != 0 && size > max_stream_size - total_size)) {
            throw std::length_error("stream exceeds max_stream_size");
        }
        total_size += size;
        write_frame_with_payload(fd, WireType::stream_request_chunk, request_id,
                                 static_cast<std::uint32_t>(size), 0, buffer.data(), size,
                                 io_timeout, deadline);
    }
    write_header_frame(fd, WireType::stream_request_end, request_id, 0, 0, io_timeout,
                       deadline);
}

} // namespace

Status run_oneshot_stream(const std::string& socket_path, const ClientOptions& options,
                          std::string_view route, const StreamReader& request_body,
                          const std::function<void(std::string_view)>& response_chunk) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > options.max_message_size || route.size() > protocol::max_wire_field) {
        throw std::length_error("route exceeds max_message_size");
    }

    const Deadline deadline = deadline_from_now(options.stream_timeout);
    FileDescriptor fd = make_socket();
    const auto address = make_address(socket_path);
    connect_nonblocking(fd.get(), address, options.connect_timeout, deadline);

    write_stream_request(fd.get(), 0, route, request_body, options.stream_chunk_size,
                         options.max_stream_size, options.io_timeout, deadline);

    BufferedReader reader(fd.get());
    HeaderBytes header{};
    reader.read(header.data(), header.size(), options.io_timeout, deadline);
    const auto decoded = protocol::decode_header(header, WireType::stream_response);
    if (decoded.request_id != 0 || decoded.arg1 > static_cast<std::uint32_t>(INT32_MAX) ||
        decoded.arg2 != 0) {
        throw Error(ErrorCode::protocol, "invalid stream response header");
    }

    std::vector<char> buffer(options.stream_chunk_size);
    std::size_t total_size = 0;
    while (true) {
        HeaderBytes chunk_header{};
        reader.read(chunk_header.data(), chunk_header.size(), options.io_timeout, deadline);
        const auto chunk = protocol::decode_header(chunk_header);
        if (chunk.type == WireType::stream_response_end) {
            if (chunk.request_id != 0 || chunk.arg1 != 0 || chunk.arg2 != 0) {
                throw Error(ErrorCode::protocol, "invalid stream end frame");
            }
            break;
        }
        if (chunk.type != WireType::stream_response_chunk || chunk.request_id != 0 ||
            chunk.arg1 == 0 || chunk.arg2 != 0) {
            throw Error(ErrorCode::protocol, "invalid stream chunk frame");
        }
        const std::size_t chunk_size = chunk.arg1;
        if (chunk_size > std::numeric_limits<std::size_t>::max() - total_size ||
            (options.max_stream_size != 0 && chunk_size > options.max_stream_size - total_size)) {
            throw Error(ErrorCode::too_large, "stream exceeds max_stream_size");
        }
        total_size += chunk_size;
        std::size_t remaining = chunk.arg1;
        while (remaining != 0) {
            const std::size_t take = std::min(remaining, buffer.size());
            reader.read(buffer.data(), take, options.io_timeout, deadline);
            if (response_chunk) {
                response_chunk(std::string_view(buffer.data(), take));
            }
            remaining -= take;
        }
    }
    return static_cast<Status>(decoded.arg1);
}

} // namespace easy_uds::detail::client
