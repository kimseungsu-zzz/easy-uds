#pragma once

// POSIX-only request capabilities.  The common Request type deliberately
// does not include this header.

#include "easy_uds/fd.hpp"
#include "easy_uds/peer_credentials.hpp"
#include "easy_uds/request_context.hpp"

#include <cstdint>

namespace easy_uds::detail {
struct RequestCapabilityStorage;

const RequestCapabilityStorage* request_capability_bridge(
    const RequestContext&) noexcept;
std::int64_t request_capability_pid(
    const RequestCapabilityStorage*) noexcept;
std::uint64_t request_capability_uid(
    const RequestCapabilityStorage*) noexcept;
std::uint64_t request_capability_gid(
    const RequestCapabilityStorage*) noexcept;
bool request_capability_peer_present(
    const RequestCapabilityStorage*) noexcept;
int request_capability_fd(const RequestCapabilityStorage*) noexcept;
} // namespace easy_uds::detail

namespace easy_uds::posix {

class RequestCapabilities {
  public:
    RequestCapabilities() noexcept = default;

    // This is a pointer-sized non-owning view.  Copying or moving it never
    // extends the RequestContext lifetime.
    RequestCapabilities(const RequestCapabilities&) noexcept = default;
    RequestCapabilities& operator=(const RequestCapabilities&) noexcept = default;
    RequestCapabilities(RequestCapabilities&&) noexcept = default;
    RequestCapabilities& operator=(RequestCapabilities&&) noexcept = default;

    [[nodiscard]] PeerCredentials peer_credentials() const noexcept {
        return PeerCredentials{
            static_cast<pid_t>(detail::request_capability_pid(bridge_)),
            static_cast<uid_t>(detail::request_capability_uid(bridge_)),
            static_cast<gid_t>(detail::request_capability_gid(bridge_)),
            detail::request_capability_peer_present(bridge_)};
    }

    [[nodiscard]] BorrowedFd received_fd() const noexcept {
        return borrow_fd(detail::request_capability_fd(bridge_));
    }

  private:
    friend RequestCapabilities request_capabilities(
        const easy_uds::RequestContext&) noexcept;

    explicit RequestCapabilities(
        const detail::RequestCapabilityStorage* bridge) noexcept
        : bridge_(bridge) {}

    const detail::RequestCapabilityStorage* bridge_ = nullptr;
};

static_assert(sizeof(RequestCapabilities) == sizeof(void*),
              "RequestCapabilities must remain a pointer-sized view");

[[nodiscard]] inline RequestCapabilities request_capabilities(
    const easy_uds::RequestContext& context) noexcept {
    return RequestCapabilities{detail::request_capability_bridge(context)};
}

} // namespace easy_uds::posix
