#pragma once

#include "easy_uds/version.hpp"

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
inline constexpr std::uint8_t version = static_cast<std::uint8_t>(easy_uds::protocol_version);
inline constexpr std::size_t header_size = 16;
inline constexpr std::size_t max_wire_field = std::numeric_limits<std::uint32_t>::max();

using HeaderBytes = std::array<unsigned char, header_size>;

enum class WireType : std::uint8_t {
    request = 1,
    response = 2,
};

struct DecodedHeader {
    std::uint32_t arg1 = 0;
    std::uint32_t arg2 = 0;
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

inline HeaderBytes encode_header(WireType type, std::uint32_t arg1, std::uint32_t arg2) noexcept {
    HeaderBytes header{};
    std::copy(magic.begin(), magic.end(), header.begin());
    header[4] = version;
    header[5] = static_cast<std::uint8_t>(type);
    put_u32(header, 8, arg1);
    put_u32(header, 12, arg2);
    return header;
}

inline DecodedHeader decode_header(const HeaderBytes& header, WireType expected_type) {
    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        throw std::runtime_error("invalid protocol magic");
    }
    if (header[4] != version) {
        throw std::runtime_error("unsupported protocol version");
    }
    if (header[5] != static_cast<std::uint8_t>(expected_type)) {
        throw std::runtime_error("unexpected protocol message type");
    }
    if (header[6] != 0 || header[7] != 0) {
        throw std::runtime_error("unsupported protocol flags");
    }
    return {get_u32(header, 8), get_u32(header, 12)};
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
