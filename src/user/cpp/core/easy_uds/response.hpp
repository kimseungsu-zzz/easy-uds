#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace easy_uds {

// Wire-transparent 32-bit status; the well-known set below mirrors the
// constants the server generates. Handlers may return any non-negative value.
using Status = std::int32_t;

inline constexpr Status status_ok = 200;
inline constexpr Status status_created = 201;
inline constexpr Status status_bad_request = 400;
inline constexpr Status status_request_timeout = 408;
inline constexpr Status status_not_found = 404;
inline constexpr Status status_conflict = 409;
inline constexpr Status status_internal_error = 500;
inline constexpr Status status_unavailable = 503;

struct Response {
    Status status = status_ok;
    std::string body;

    // The smallest beginner convenience: preserve aggregate construction for
    // explicit statuses while making the common successful reply read
    // naturally. This is inline and layout-neutral.
    [[nodiscard]] static Response ok(std::string body = {}) {
        return Response{status_ok, std::move(body)};
    }
};

} // namespace easy_uds
