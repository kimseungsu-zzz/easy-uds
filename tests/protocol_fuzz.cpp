#include "protocol/codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace easy_uds::detail::protocol;

    if (size < header_size) {
        return 0;
    }

    HeaderBytes header{};
    std::copy_n(data, header.size(), header.begin());

    const std::array<WireType, 8> types{
        WireType::request,
        WireType::response,
        WireType::stream_request,
        WireType::stream_request_chunk,
        WireType::stream_request_end,
        WireType::stream_response,
        WireType::stream_response_chunk,
        WireType::stream_response_end,
    };
    for (const WireType type : types) {
        try {
            const DecodedHeader decoded = decode_header(header, type);
            if (type == WireType::request) {
                const std::size_t max_message_size =
                    size > header_size ? std::max<std::size_t>(1, static_cast<std::size_t>(data[header_size]) * 4096U)
                                       : 1024U * 1024U;
                try {
                    validate_request_lengths(decoded.arg1, decoded.arg2, max_message_size);
                } catch (...) {
                }
            }

            // Round-trip through the encoder, exercising request-id preservation.
            const HeaderBytes round_trip =
                encode_header(type, decoded.request_id, decoded.arg1, decoded.arg2, decoded.flags);
            const DecodedHeader decoded_again = decode_header(round_trip, type);
            if (decoded_again.request_id != decoded.request_id ||
                decoded_again.flags != decoded.flags) {
                return 1;
            }
        } catch (...) {
        }
    }

    return 0;
}
