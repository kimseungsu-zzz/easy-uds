#include "protocol/codec.hpp"

#include <array>
#include <cstdint>
#include <iostream>

int main() {
    using namespace easy_uds::detail::protocol;
    const auto header = encode_header(WireType::request, 0x01020304U,
                                      0xA0B0C0D0U, 0x10203040U);
    const HeaderBytes expected{
        'E', 'U', 'D', 'S', 2, static_cast<unsigned char>(WireType::request),
        0, 0, 0x01, 0x02, 0x03, 0x04, 0xA0, 0xB0, 0xC0, 0xD0,
        0x10, 0x20, 0x30, 0x40};
    if (header != expected) {
        std::cerr << "protocol v2 header bytes changed\n";
        return 1;
    }
    const auto decoded = decode_header(header, WireType::request);
    if (decoded.request_id != 0x01020304U || decoded.arg1 != 0xA0B0C0D0U ||
        decoded.arg2 != 0x10203040U || decoded.flags != 0) {
        std::cerr << "protocol v2 golden header did not decode\n";
        return 1;
    }
    return 0;
}
