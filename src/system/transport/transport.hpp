#pragma once

#include "easy_uds/options.hpp"
#include "easy_uds/response.hpp"
#include "easy_uds/stream.hpp"

#include "io.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace easy_uds::detail::client {

#if defined(__GNUC__) || defined(__clang__)
#define EASY_UDS_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define EASY_UDS_DETAIL_HIDDEN
#endif

inline void validate_request_lengths(std::string_view route, std::string_view body,
                                     std::size_t max_message_size) {
    protocol::validate_request_lengths(route.size(), body.size(), max_message_size);
}

inline void write_request_frame(NativeSocket fd, std::uint32_t request_id, std::string_view route,
                                std::string_view body,
                                std::chrono::milliseconds io_timeout, Deadline deadline) {
    const protocol::HeaderBytes header = protocol::encode_header(
        protocol::WireType::request, request_id,
        static_cast<std::uint32_t>(route.size()),
        static_cast<std::uint32_t>(body.size()));
    std::array<iovec, 3> parts{{
        {const_cast<unsigned char*>(header.data()), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact(fd, parts.data(), parts.size(), io_timeout, deadline);
}

EASY_UDS_DETAIL_HIDDEN Status run_oneshot_stream(
    const std::string& socket_path, const ClientOptions& options, std::string_view route,
    const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk);

#undef EASY_UDS_DETAIL_HIDDEN

} // namespace easy_uds::detail::client
