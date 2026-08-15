#include "easy_uds/session.hpp"

#include "../../../system/runtime/session_engine.hpp"
#include "../../../system/transport/transport.hpp"

#include <stdexcept>
#include <utility>

namespace easy_uds {

Session::Session(std::string socket_path, ClientOptions options)
    : state_(std::make_unique<detail::SessionState>(std::move(socket_path), options)) {
    const detail::Deadline deadline = detail::deadline_from_now(state_->options.request_timeout);
    state_->fd = detail::make_socket();
    const sockaddr_un address = detail::make_address(state_->socket_path);
    detail::connect_nonblocking(state_->fd.get(), address, state_->options.connect_timeout,
                                deadline);
    state_->reader_thread =
        std::thread([state = state_.get()] { detail::session_reader_loop(state); });
}

Session::~Session() {
    detail::shutdown_session_state(state_);
}

Session::Session(Session&& other) noexcept = default;
Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        detail::shutdown_session_state(state_);
        state_ = std::move(other.state_);
    }
    return *this;
}

SessionStatus Session::status() const noexcept {
    if (!state_) {
        return SessionStatus::moved_from;
    }
    return state_->broken.load(std::memory_order_acquire) ? SessionStatus::broken
                                                          : SessionStatus::active;
}

bool Session::valid() const noexcept {
    return status() == SessionStatus::active;
}

SessionStats Session::stats() const {
    if (!state_) {
        throw std::logic_error("session has been moved from");
    }
    SessionStats snapshot;
    SessionStatsCounters counters;
    for (std::size_t index = 0;
         index < detail::SessionState::inflight_shard_count; ++index) {
        auto& shard = state_->inflight_shards[index];
        auto lock = detail::acquire_session_lock(
            shard.mutex, detail::SessionLockKind::caller_table);
        snapshot.inflight_requests += shard.inflight.size();
        if (state_->counters) {
            const auto& shard_counters = (*state_->counters)[index];
            counters.requests_started += shard_counters.requests_started;
            counters.requests_completed += shard_counters.requests_completed;
            counters.requests_timed_out += shard_counters.requests_timed_out;
            counters.requests_failed += shard_counters.requests_failed;
        }
    }
    if (state_->counters) {
        snapshot.counters = counters;
    }
    return snapshot;
}

Response Session::request(std::string_view route, std::string_view body) {
    if (!state_) {
        throw std::logic_error("session has been moved from");
    }
    detail::client::validate_request_lengths(route, body, state_->options.max_message_size);
    if (state_->broken.load(std::memory_order_acquire)) {
        throw Error(ErrorCode::closed, "session connection is no longer usable");
    }

    detail::SessionState::Slot slot;
    std::uint32_t request_id = 0;
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    std::uint64_t request_id_probes = 0;
#endif
    for (;;) {
        request_id = state_->next_id.fetch_add(1, std::memory_order_relaxed);
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
        ++request_id_probes;
#endif
        if (request_id == 0) {
            continue;
        }
        auto& shard = state_->shard_for(request_id);
        auto lock = detail::acquire_session_lock(
            shard.mutex, detail::SessionLockKind::caller_table);
        if (state_->broken.load(std::memory_order_acquire)) {
            throw Error(ErrorCode::closed, "session connection is no longer usable");
        }
        if (shard.inflight.find(request_id) != shard.inflight.end()) {
            continue;
        }
        shard.insert(request_id, &slot);
        if (auto* counters = state_->counters_for(request_id)) {
            ++counters->requests_started;
        }
        break;
    }
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    detail::session_trace_counters.request_id_probes.fetch_add(
        request_id_probes, std::memory_order_relaxed);
#endif

    const detail::Deadline deadline = detail::deadline_from_now(state_->options.request_timeout);
    try {
        auto lock = detail::acquire_session_lock(
            state_->send_mutex, detail::SessionLockKind::send);
        detail::client::write_request_frame(state_->fd.get(), request_id, route, body,
                                            state_->options.io_timeout, deadline);
    } catch (...) {
        state_->broken.store(true, std::memory_order_release);
        {
            auto& shard = state_->shard_for(request_id);
            auto lock = detail::acquire_session_lock(
                shard.mutex, detail::SessionLockKind::caller_table);
            if (auto* counters = state_->counters_for(request_id)) {
                ++counters->requests_failed;
            }
            shard.erase(request_id);
        }
        (void)::shutdown(state_->fd.get(), SHUT_RDWR);
        throw;
    }

    const detail::Deadline spin_deadline =
        detail::Clock::now() + detail::session_spin_duration;
    while (!slot.done.load(std::memory_order_acquire)) {
        const detail::Deadline now = detail::Clock::now();
        if (now >= spin_deadline || now >= deadline) {
            break;
        }
        detail::session_spin_hint();
    }

    bool timed_out = false;
    {
        auto slot_lock = detail::acquire_session_lock(
            slot.mutex, detail::SessionLockKind::waiter_slot);
        if (!slot.done.load(std::memory_order_acquire)) {
#ifdef EASY_UDS_TRACE_SPIN_MISS
            detail::session_spin_miss_count.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
            detail::session_trace_counters.condition_waits.fetch_add(
                1, std::memory_order_relaxed);
#endif
            timed_out = !slot.cv.wait_until(slot_lock, deadline, [&slot] {
                return slot.done.load(std::memory_order_acquire);
            });
        }
    }
    {
        auto& shard = state_->shard_for(request_id);
        auto lock = detail::acquire_session_lock(
            shard.mutex, detail::SessionLockKind::caller_table);
        if (auto* counters = state_->counters_for(request_id)) {
            if (timed_out) {
                ++counters->requests_timed_out;
            } else if (slot.error) {
                ++counters->requests_failed;
            } else {
                ++counters->requests_completed;
            }
        }
        shard.erase(request_id);
    }
    if (timed_out) {
        state_->broken.store(true, std::memory_order_release);
        (void)::shutdown(state_->fd.get(), SHUT_RDWR);
        detail::throw_system_error("request timed out", ETIMEDOUT);
    }
    const std::exception_ptr error = slot.error;
    Response response = std::move(slot.response);
    if (error) {
        std::rethrow_exception(error);
    }
    return response;
}

Status Session::request_stream(
    std::string_view route, const StreamReader& request_body,
    const std::function<void(std::string_view)>& response_chunk) {
    if (!state_) {
        throw std::logic_error("session has been moved from");
    }
    return detail::client::run_oneshot_stream(state_->socket_path, state_->options, route,
                                              request_body, response_chunk);
}

} // namespace easy_uds
