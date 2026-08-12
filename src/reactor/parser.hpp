#pragma once

#include "core.hpp"

#include <memory>

namespace easy_uds::detail {

// Reads available bytes and dispatches every complete request frame.
void consume(const std::shared_ptr<ServerState>& state,
             const std::shared_ptr<ReactorConnection>& connection);

} // namespace easy_uds::detail
