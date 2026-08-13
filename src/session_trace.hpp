#pragma once

#include <atomic>
#include <cstdint>

namespace easy_uds::detail {

#ifdef EASY_UDS_TRACE_SESSION_CONTENTION

struct SessionTraceCounters {
    std::atomic<std::uint64_t> send_lock_wait_ns{0};
    std::atomic<std::uint64_t> send_lock_acquisitions{0};
    std::atomic<std::uint64_t> caller_table_lock_wait_ns{0};
    std::atomic<std::uint64_t> caller_table_lock_acquisitions{0};
    std::atomic<std::uint64_t> reader_table_lock_wait_ns{0};
    std::atomic<std::uint64_t> reader_table_lock_acquisitions{0};
    std::atomic<std::uint64_t> reader_slot_lock_wait_ns{0};
    std::atomic<std::uint64_t> reader_slot_lock_acquisitions{0};
    std::atomic<std::uint64_t> waiter_slot_lock_wait_ns{0};
    std::atomic<std::uint64_t> waiter_slot_lock_acquisitions{0};
    std::atomic<std::uint64_t> request_id_probes{0};
    std::atomic<std::uint64_t> response_lookups{0};
    std::atomic<std::uint64_t> waiter_notifications{0};
    std::atomic<std::uint64_t> condition_waits{0};

    void reset() noexcept {
        send_lock_wait_ns.store(0, std::memory_order_relaxed);
        send_lock_acquisitions.store(0, std::memory_order_relaxed);
        caller_table_lock_wait_ns.store(0, std::memory_order_relaxed);
        caller_table_lock_acquisitions.store(0, std::memory_order_relaxed);
        reader_table_lock_wait_ns.store(0, std::memory_order_relaxed);
        reader_table_lock_acquisitions.store(0, std::memory_order_relaxed);
        reader_slot_lock_wait_ns.store(0, std::memory_order_relaxed);
        reader_slot_lock_acquisitions.store(0, std::memory_order_relaxed);
        waiter_slot_lock_wait_ns.store(0, std::memory_order_relaxed);
        waiter_slot_lock_acquisitions.store(0, std::memory_order_relaxed);
        request_id_probes.store(0, std::memory_order_relaxed);
        response_lookups.store(0, std::memory_order_relaxed);
        waiter_notifications.store(0, std::memory_order_relaxed);
        condition_waits.store(0, std::memory_order_relaxed);
    }
};

extern SessionTraceCounters session_trace_counters;

#endif

} // namespace easy_uds::detail
