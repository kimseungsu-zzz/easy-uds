#pragma once

#include <cstdint>
#include <string>

namespace easy_uds {

struct Request {
    Request() = default;

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    Request(Request&&) noexcept = default;
    Request& operator=(Request&&) noexcept = default;

    std::string route;
    std::string body;
    // Correlation id for the multiplexed protocol. The client assigns it per
    // in-flight request; responses to different requests may arrive in any
    // order. Handlers that mutate shared state must not rely on it being
    // sequential.
    std::uint32_t request_id = 0;
};

} // namespace easy_uds
