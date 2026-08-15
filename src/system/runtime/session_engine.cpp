#include "session_engine.hpp"

#include "../transport/transport.hpp"

#include <array>
#include <exception>
#include <stdexcept>

namespace easy_uds::detail {

#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
SessionTraceCounters session_trace_counters;
#endif

std::unique_lock<std::mutex> acquire_session_lock(std::mutex& mutex,
                                                   SessionLockKind kind) {
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    const auto started = Clock::now();
    std::unique_lock<std::mutex> lock(mutex);
    const auto wait_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started)
            .count());
    std::atomic<std::uint64_t>* total_wait = nullptr;
    std::atomic<std::uint64_t>* acquisitions = nullptr;
    switch (kind) {
    case SessionLockKind::send:
        total_wait = &session_trace_counters.send_lock_wait_ns;
        acquisitions = &session_trace_counters.send_lock_acquisitions;
        break;
    case SessionLockKind::caller_table:
        total_wait = &session_trace_counters.caller_table_lock_wait_ns;
        acquisitions = &session_trace_counters.caller_table_lock_acquisitions;
        break;
    case SessionLockKind::reader_table:
        total_wait = &session_trace_counters.reader_table_lock_wait_ns;
        acquisitions = &session_trace_counters.reader_table_lock_acquisitions;
        break;
    case SessionLockKind::reader_slot:
        total_wait = &session_trace_counters.reader_slot_lock_wait_ns;
        acquisitions = &session_trace_counters.reader_slot_lock_acquisitions;
        break;
    case SessionLockKind::waiter_slot:
        total_wait = &session_trace_counters.waiter_slot_lock_wait_ns;
        acquisitions = &session_trace_counters.waiter_slot_lock_acquisitions;
        break;
    }
    total_wait->fetch_add(wait_ns, std::memory_order_relaxed);
    acquisitions->fetch_add(1, std::memory_order_relaxed);
    return lock;
#else
    (void)kind;
    return std::unique_lock<std::mutex>(mutex);
#endif
}

#ifdef EASY_UDS_TRACE_SPIN_MISS
std::atomic<std::size_t> session_spin_miss_count{0};
#endif

void session_reader_loop(SessionState* state) {
    try {
        BufferedReader reader(state->fd.get());
        while (!state->reader_stop.load(std::memory_order_relaxed)) {
            HeaderBytes header{};
            if (!reader.buffered()) {
                wait_for_io(state->fd.get(), POLLIN, std::chrono::milliseconds{0},
                            Deadline::max(), "receive timed out");
            }
            reader.read(header.data(), header.size(), state->options.io_timeout,
                        Deadline::max());
            const auto decoded = protocol::decode_header(header, WireType::response);
            if (decoded.arg1 > static_cast<std::uint32_t>(INT32_MAX)) {
                throw Error(ErrorCode::protocol, "response status_code is out of range");
            }
            if (decoded.arg2 > state->options.max_message_size) {
                throw Error(ErrorCode::too_large, "response exceeds max_message_size");
            }
            Response response;
            response.status = static_cast<Status>(decoded.arg1);
            response.body.resize(decoded.arg2);
            reader.read(response.body.data(), response.body.size(),
                        state->options.io_timeout, Deadline::max());

            auto& shard = state->shard_for(decoded.request_id);
            auto lock = acquire_session_lock(shard.mutex, SessionLockKind::reader_table);
            const auto it = shard.inflight.find(decoded.request_id);
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
            session_trace_counters.response_lookups.fetch_add(1,
                                                               std::memory_order_relaxed);
#endif
            if (it == shard.inflight.end()) {
                throw Error(ErrorCode::protocol, "unexpected response request_id");
            }
            auto* const slot = it->second;
            auto slot_lock = acquire_session_lock(slot->mutex, SessionLockKind::reader_slot);
            if (slot->done.load(std::memory_order_acquire)) {
                throw Error(ErrorCode::protocol, "duplicate response request_id");
            }
            slot->response = std::move(response);
            slot->done.store(true, std::memory_order_release);
            slot->cv.notify_one();
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
            session_trace_counters.waiter_notifications.fetch_add(1,
                                                                   std::memory_order_relaxed);
#endif
        }
    } catch (...) {
        const std::exception_ptr error = std::current_exception();
        state->broken.store(true, std::memory_order_release);
        for (auto& shard : state->inflight_shards) {
            auto lock = acquire_session_lock(shard.mutex, SessionLockKind::reader_table);
            for (auto& entry : shard.inflight) {
                auto* const slot = entry.second;
                auto slot_lock = acquire_session_lock(slot->mutex, SessionLockKind::reader_slot);
                if (!slot->done.load(std::memory_order_acquire)) {
                    slot->error = error;
                    slot->done.store(true, std::memory_order_release);
                    slot->cv.notify_one();
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
                    session_trace_counters.waiter_notifications.fetch_add(
                        1, std::memory_order_relaxed);
#endif
                }
            }
        }
    }
}

void shutdown_session_state(std::unique_ptr<SessionState>& state) noexcept {
    if (!state) {
        return;
    }
    state->reader_stop.store(true, std::memory_order_relaxed);
    if (state->fd.get() >= 0) {
        socket_lifecycle::shutdown(state->fd.get());
    }
    if (state->reader_thread.joinable()) {
        state->reader_thread.join();
    }
    state.reset();
}

} // namespace easy_uds::detail
