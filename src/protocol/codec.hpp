#pragma once

#include "easy_uds/error.hpp"

// Protocol version 2 wire codec boundary.
//
// Framing: every message begins with a fixed 20-byte, big-endian header:
//
//   offset  size  field
//   0       4     magic "EUDS"
//   4       1     version = 2
//   5       1     message type
//   6       2     flags (bit 0 = one descriptor on a fixed one-shot request)
//   8       4     request id
//   12      4     argument 1
//   16      4     argument 2
//
// The request id correlates responses with in-flight requests on a
// multiplexed connection (responses may arrive out of order). Stream frames
// carry the same id so concurrent streams are disambiguated.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <arpa/inet.h>

namespace easy_uds::detail::protocol {

inline constexpr std::array<unsigned char, 4> magic{'E', 'U', 'D', 'S'};
inline constexpr std::uint8_t version = 2;
inline constexpr std::size_t header_size = 20;
inline constexpr std::size_t header_field_offset = 8;  // request id
inline constexpr std::size_t arg1_offset = 12;
inline constexpr std::size_t arg2_offset = 16;
inline constexpr std::uint32_t max_wire_field = std::numeric_limits<std::uint32_t>::max();

using HeaderBytes = std::array<unsigned char, header_size>;

enum class WireType : std::uint8_t {
    request = 1,
    response = 2,
    stream_request = 3,
    stream_request_chunk = 4,
    stream_request_end = 5,
    stream_response = 6,
    stream_response_chunk = 7,
    stream_response_end = 8,
};

// Reserved-flags bits (header bytes 6-7, big-endian). Bit 0 marks a frame that
// carries one descriptor as SCM_RIGHTS ancillary data, delivered with the
// frame's bytes. Only this bit is understood; any other bit is rejected.
inline constexpr std::uint16_t carries_fd_flag = 0x0001U;

struct DecodedHeader {
    WireType type = WireType::request;
    std::uint32_t request_id = 0;
    std::uint32_t arg1 = 0;
    std::uint32_t arg2 = 0;
    std::uint16_t flags = 0;
};

inline void put_u32(HeaderBytes& header, std::size_t offset, std::uint32_t value) noexcept {
    const std::uint32_t wire_value = htonl(value);
    std::memcpy(header.data() + offset, &wire_value, sizeof(wire_value));
}

inline std::uint32_t get_u32(const HeaderBytes& header, std::size_t offset) noexcept {
    std::uint32_t wire_value = 0;
    std::memcpy(&wire_value, header.data() + offset, sizeof(wire_value));
    return ntohl(wire_value);
}

inline HeaderBytes encode_header(WireType type, std::uint32_t request_id, std::uint32_t arg1,
                                 std::uint32_t arg2, std::uint16_t flags = 0) noexcept {
    HeaderBytes header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    header[4] = version;
    header[5] = static_cast<std::uint8_t>(type);
    header[6] = static_cast<unsigned char>((flags >> 8) & 0xFFU);
    header[7] = static_cast<unsigned char>(flags & 0xFFU);
    put_u32(header, header_field_offset, request_id);
    put_u32(header, arg1_offset, arg1);
    put_u32(header, arg2_offset, arg2);
    return header;
}

inline DecodedHeader decode_header(const HeaderBytes& header) {
    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        throw Error(ErrorCode::protocol, "invalid protocol magic");
    }
    if (header[4] != version) {
        throw Error(ErrorCode::protocol, "unsupported protocol version");
    }
    const std::uint16_t flags =
        (static_cast<std::uint16_t>(header[6]) << 8) | static_cast<std::uint16_t>(header[7]);
    if ((flags & ~carries_fd_flag) != 0) {
        throw Error(ErrorCode::protocol, "unsupported protocol flags");
    }
    const auto raw_type = header[5];
    if (raw_type < static_cast<std::uint8_t>(WireType::request) ||
        raw_type > static_cast<std::uint8_t>(WireType::stream_response_end)) {
        throw Error(ErrorCode::protocol, "unknown protocol message type");
    }
    const std::uint32_t request_id = get_u32(header, header_field_offset);
    if ((flags & carries_fd_flag) != 0 &&
        (raw_type != static_cast<std::uint8_t>(WireType::request) || request_id != 0)) {
        throw Error(ErrorCode::protocol,
                    "descriptor flag is only valid on one-shot fixed requests");
    }
    return {static_cast<WireType>(raw_type), request_id, get_u32(header, arg1_offset),
            get_u32(header, arg2_offset), flags};
}

inline DecodedHeader decode_header(const HeaderBytes& header, WireType expected_type) {
    const auto decoded = decode_header(header);
    if (decoded.type != expected_type) {
        throw Error(ErrorCode::protocol, "unexpected protocol message type");
    }
    return decoded;
}

inline void validate_request_lengths(std::size_t route_size, std::size_t body_size, std::size_t max_message_size) {
    if (route_size == 0) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route_size > max_wire_field || body_size > max_wire_field || route_size > max_message_size ||
        body_size > max_message_size - route_size) {
        throw std::length_error("request exceeds max_message_size");
    }
}

} // namespace easy_uds::detail::protocol
