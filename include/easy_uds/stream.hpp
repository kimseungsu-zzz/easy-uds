#pragma once

#include "easy_uds/response.hpp"

#include <cstddef>
#include <functional>

namespace easy_uds {

// A pull-based byte source. Implementations fill at most `capacity` bytes and
// return the number produced. Returning zero marks end-of-stream.
using StreamReader = std::function<std::size_t(char* buffer, std::size_t capacity)>;

// A streamed response is consumed immediately after the handler returns. Its
// reader must own (or otherwise outlive) every resource it captures.
struct StreamResponse {
    Status status = status_ok;
    StreamReader body;
};

} // namespace easy_uds
