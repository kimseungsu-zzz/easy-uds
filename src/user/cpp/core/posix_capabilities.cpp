#include "easy_uds/posix.hpp"

namespace easy_uds::detail {

const RequestCapabilityStorage* request_capability_bridge(
    const RequestContext& context) noexcept {
    return context.capability_bridge();
}

} // namespace easy_uds::detail
