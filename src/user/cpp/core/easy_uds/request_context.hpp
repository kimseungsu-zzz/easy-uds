#pragma once

#include "easy_uds/request.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace easy_uds {

class RequestContext;

namespace detail {
struct RequestContextFactory;
struct RequestCapabilityStorage;
const RequestCapabilityStorage* request_capability_bridge(
    const RequestContext&) noexcept;
}

// Read-only metadata and cooperative-stop state for one handler invocation.
// The view is valid only until the handler returns, cannot be copied, and must
// not be retained. POSIX peer/descriptor capabilities are exposed separately
// by easy_uds/posix.hpp. It observes server state; it does not probe peer
// liveness or interrupt user code.
class RequestContext {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    RequestContext(const RequestContext&) = delete;
    RequestContext& operator=(const RequestContext&) = delete;
    RequestContext(RequestContext&&) = delete;
    RequestContext& operator=(RequestContext&&) = delete;

    [[nodiscard]] std::uint32_t request_id() const noexcept {
        return request_->request_id;
    }

    // Time at which the server observed the first byte of this request frame.
    [[nodiscard]] TimePoint arrival_time() const noexcept {
        return arrival_time_;
    }

    // Absolute request deadline. An empty value means request_timeout is
    // disabled for this request.
    [[nodiscard]] std::optional<TimePoint> deadline() const noexcept {
        if (deadline_ == TimePoint::max()) {
            return std::nullopt;
        }
        return deadline_;
    }

    [[nodiscard]] bool deadline_expired() const noexcept {
        return deadline_ != TimePoint::max() && Clock::now() >= deadline_;
    }

    // True after the server has marked this connection unusable or closing.
    // False means no close has been observed; it is not a liveness guarantee.
    [[nodiscard]] bool connection_closing() const noexcept {
        return connection_closing_->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool server_stopping() const noexcept {
        return !server_running_->load(std::memory_order_acquire);
    }

    // Cooperative cancellation signal. Handlers decide when and how to stop.
    [[nodiscard]] bool stop_requested() const noexcept {
        return connection_closing() || server_stopping() || deadline_expired();
    }

  private:
    friend struct detail::RequestContextFactory;
    friend const detail::RequestCapabilityStorage*
    detail::request_capability_bridge(const RequestContext&) noexcept;

    RequestContext(const Request& request, TimePoint arrival_time,
                   TimePoint deadline,
                   const std::atomic<bool>& connection_closing,
                   const std::atomic<bool>& server_running,
                   const detail::RequestCapabilityStorage* capability_bridge) noexcept
        : request_(&request), arrival_time_(arrival_time), deadline_(deadline),
          connection_closing_(&connection_closing),
          server_running_(&server_running), capability_bridge_(capability_bridge) {}

    [[nodiscard]] const detail::RequestCapabilityStorage*
    capability_bridge() const noexcept {
        return capability_bridge_;
    }

    const Request* request_;
    TimePoint arrival_time_;
    TimePoint deadline_;
    const std::atomic<bool>* connection_closing_;
    const std::atomic<bool>* server_running_;
    const detail::RequestCapabilityStorage* capability_bridge_;
};

} // namespace easy_uds
