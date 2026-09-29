#include "request_capabilities.hpp"

#include "easy_uds/request_context.hpp"

namespace easy_uds::detail {

const RequestCapabilityStorage* request_capability_bridge(
    const easy_uds::RequestContext& context) noexcept {
    return context.capability_bridge();
}

std::int64_t request_capability_pid(
    const RequestCapabilityStorage* bridge) noexcept {
    return bridge == nullptr ? -1 : bridge->peer.pid;
}

std::uint64_t request_capability_uid(
    const RequestCapabilityStorage* bridge) noexcept {
    return bridge == nullptr ? static_cast<std::uint64_t>(-1)
                             : bridge->peer.uid;
}

std::uint64_t request_capability_gid(
    const RequestCapabilityStorage* bridge) noexcept {
    return bridge == nullptr ? static_cast<std::uint64_t>(-1)
                             : bridge->peer.gid;
}

bool request_capability_peer_present(
    const RequestCapabilityStorage* bridge) noexcept {
    return bridge != nullptr && bridge->peer.present;
}

int request_capability_fd(const RequestCapabilityStorage* bridge) noexcept {
    return bridge == nullptr ? -1 : bridge->received_fd.native_fd();
}

#if defined(_WIN32)
void* request_capability_windows_handle(
    const RequestCapabilityStorage* bridge) noexcept {
    return bridge == nullptr ? nullptr : bridge->received_handle.get();
}
#endif

} // namespace easy_uds::detail
