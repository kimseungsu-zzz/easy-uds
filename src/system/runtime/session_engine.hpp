#pragma once

#include "easy_uds/session.hpp"

#include "../transport/io.hpp"
#include "trace.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace easy_uds::detail {

inline void session_spin_hint() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

#ifndef EASY_UDS_SESSION_SPIN_US
#define EASY_UDS_SESSION_SPIN_US 100
#endif

#ifndef EASY_UDS_SESSION_INFLIGHT_SHARDS
#define EASY_UDS_SESSION_INFLIGHT_SHARDS 16
#endif

static_assert(EASY_UDS_SESSION_SPIN_US >= 0,
              "session spin duration must not be negative");
static_assert(EASY_UDS_SESSION_INFLIGHT_SHARDS >= 1 &&
                  EASY_UDS_SESSION_INFLIGHT_SHARDS <= 64,
              "session in-flight shard count must be from 1 to 64");
inline constexpr auto session_spin_duration =
    std::chrono::microseconds{EASY_UDS_SESSION_SPIN_US};

enum class SessionLockKind {
    send,
    caller_table,
    reader_table,
    reader_slot,
    waiter_slot,
};

std::unique_lock<std::mutex> acquire_session_lock(std::mutex& mutex,
                                                   SessionLockKind kind);

#ifdef EASY_UDS_TRACE_SPIN_MISS
extern std::atomic<std::size_t> session_spin_miss_count;
#endif

struct SessionState {
    static constexpr std::size_t inflight_shard_count =
        EASY_UDS_SESSION_INFLIGHT_SHARDS;

    struct alignas(64) CounterShard {
        std::uint64_t requests_started = 0;
        std::uint64_t requests_completed = 0;
        std::uint64_t requests_timed_out = 0;
        std::uint64_t requests_failed = 0;
    };

    explicit SessionState(std::string socket_path, ClientOptions options)
        : socket_path(std::move(socket_path)), options(options) {
        if (options.stats == StatsMode::basic) {
            counters = std::make_unique<
                std::array<CounterShard, inflight_shard_count>>();
        }
    }

    const std::string socket_path;
    ClientOptions options;

    FileDescriptor fd;
    std::mutex send_mutex;
    std::atomic<bool> broken{false};
    std::atomic<bool> reader_stop{false};
    std::atomic<std::uint32_t> next_id{1};

    struct Slot {
        std::atomic<bool> done{false};
        std::mutex mutex;
        std::condition_variable cv;
        Response response;
        std::exception_ptr error;
    };
    using InflightMap = std::unordered_map<std::uint32_t, Slot*>;
    static constexpr std::size_t cached_inflight_slots = 64;
    static constexpr std::size_t cached_slots_per_shard =
        (cached_inflight_slots + inflight_shard_count - 1) /
        inflight_shard_count;

    struct alignas(64) InflightShard {
        InflightShard() {
            inflight.reserve(cached_slots_per_shard);
            free_inflight_nodes.reserve(cached_slots_per_shard);
        }

        std::mutex mutex;
        InflightMap inflight;
        std::vector<InflightMap::node_type> free_inflight_nodes;

        void insert(std::uint32_t request_id, Slot* slot) {
            if (free_inflight_nodes.empty()) {
                inflight.emplace(request_id, slot);
                return;
            }
            auto node = std::move(free_inflight_nodes.back());
            free_inflight_nodes.pop_back();
            node.key() = request_id;
            node.mapped() = slot;
            const auto result = inflight.insert(std::move(node));
            if (!result.inserted) {
                throw std::logic_error("duplicate session request_id");
            }
        }

        void erase(std::uint32_t request_id) {
            auto node = inflight.extract(request_id);
            if (!node.empty() && free_inflight_nodes.size() < cached_slots_per_shard) {
                node.mapped() = nullptr;
                free_inflight_nodes.push_back(std::move(node));
            }
        }
    };

    std::array<InflightShard, inflight_shard_count> inflight_shards;
    std::unique_ptr<std::array<CounterShard, inflight_shard_count>> counters;

    [[nodiscard]] InflightShard& shard_for(std::uint32_t request_id) noexcept {
        return inflight_shards[request_id % inflight_shard_count];
    }

    [[nodiscard]] CounterShard* counters_for(std::uint32_t request_id) noexcept {
        return counters ? &(*counters)[request_id % inflight_shard_count]
                        : nullptr;
    }

    std::thread reader_thread;
};

void session_reader_loop(SessionState* state);
void shutdown_session_state(std::unique_ptr<SessionState>& state) noexcept;

} // namespace easy_uds::detail
