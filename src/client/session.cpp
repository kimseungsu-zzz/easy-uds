#include "easy_uds/session.hpp"

#include "../detail/io.hpp"
#include "trace.hpp"
#include "transport.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace easy_uds {
namespace {

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

static_assert(EASY_UDS_SESSION_SPIN_US >= 0, "session spin duration must not be negative");
static_assert(EASY_UDS_SESSION_INFLIGHT_SHARDS >= 1 && EASY_UDS_SESSION_INFLIGHT_SHARDS <= 64,
              "session in-flight shard count must be from 1 to 64");
constexpr auto session_spin_duration = std::chrono::microseconds{EASY_UDS_SESSION_SPIN_US};

} // namespace

// ---- Session ---------------------------------------------------------------

namespace detail {

#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
SessionTraceCounters session_trace_counters;
#endif

enum class SessionLockKind {
    send,
    caller_table,
    reader_table,
    reader_slot,
    waiter_slot,
};

std::unique_lock<std::mutex> acquire_session_lock(std::mutex& mutex, SessionLockKind kind) {
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    const auto started = Clock::now();
    std::unique_lock<std::mutex> lock(mutex);
    const auto wait_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
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
// Diagnostic (experimental): number of session requests whose spin window
// expired before the response landed, forcing the caller to fall through to
// the condition-variable wait. Compiled only when EASY_UDS_TRACE_SPIN_MISS is
// defined; the default build has no such counter.
std::atomic<std::size_t> session_spin_miss_count{0};
#endif

struct SessionState {
    static constexpr std::size_t inflight_shard_count = EASY_UDS_SESSION_INFLIGHT_SHARDS;

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

    // The shards own no slots: each request keeps its slot alive on its own
    // stack until it erases the entry while holding the matching shard mutex.
    // Per-slot notification wakes only the matching caller.
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
        (cached_inflight_slots + inflight_shard_count - 1) / inflight_shard_count;

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
    // Separate allocation preserves InflightShard's cache-line-rounded size.
    // Updates happen only under the corresponding existing shard mutex.
    std::unique_ptr<std::array<CounterShard, inflight_shard_count>> counters;

    [[nodiscard]] InflightShard& shard_for(std::uint32_t request_id) noexcept {
        return inflight_shards[request_id % inflight_shard_count];
    }

    [[nodiscard]] CounterShard* counters_for(std::uint32_t request_id) noexcept {
        return counters ? &(*counters)[request_id % inflight_shard_count]
                        : nullptr;
    }

    // Reusing extracted C++17 map nodes removes steady-state allocator traffic
    // while each shard preserves O(1) request-id lookup.
    std::thread reader_thread;
};

void session_reader_loop(detail::SessionState* state) {
    try {
        BufferedReader reader(state->fd.get());
        while (!state->reader_stop.load(std::memory_order_relaxed)) {
            HeaderBytes header{};
            // An idle persistent session is not an I/O operation in progress.
            // Wait indefinitely for the first response byte; once the socket
            // becomes readable, BufferedReader applies io_timeout to a partial
            // header and the payload. Pending requests retain their independent
            // absolute request_timeout and shut this socket down on expiry.
            if (!reader.buffered()) {
                wait_for_io(state->fd.get(), POLLIN, std::chrono::milliseconds{0}, Deadline::max(),
                            "receive timed out");
            }
            reader.read(header.data(), header.size(), state->options.io_timeout, Deadline::max());
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
            reader.read(response.body.data(), response.body.size(), state->options.io_timeout, Deadline::max());

            {
                auto& shard = state->shard_for(decoded.request_id);
                auto lock = acquire_session_lock(shard.mutex, SessionLockKind::reader_table);
                const auto it = shard.inflight.find(decoded.request_id);
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
                session_trace_counters.response_lookups.fetch_add(1, std::memory_order_relaxed);
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
                session_trace_counters.waiter_notifications.fetch_add(1, std::memory_order_relaxed);
#endif
            }
        }
    } catch (...) {
        // Connection failed: break every pending request and mark the session
        // permanently unusable.
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
                    session_trace_counters.waiter_notifications.fetch_add(1, std::memory_order_relaxed);
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
    // shutdown() wakes the reader thread's blocked recv; join before the
    // SessionState (and therefore the std::thread) is destroyed.
    if (state->fd.get() >= 0) {
        (void)::shutdown(state->fd.get(), SHUT_RDWR);
    }
    if (state->reader_thread.joinable()) {
        state->reader_thread.join();
    }
    state.reset();
}

} // namespace detail

Session::Session(std::string socket_path, ClientOptions options)
    : state_(std::make_unique<detail::SessionState>(std::move(socket_path), options)) {
    const detail::Deadline deadline = detail::deadline_from_now(state_->options.request_timeout);
    state_->fd = detail::make_socket();
    const sockaddr_un address = detail::make_address(state_->socket_path);
    detail::connect_nonblocking(state_->fd.get(), address, state_->options.connect_timeout,
                                deadline);
    state_->reader_thread = std::thread([state = state_.get()] { detail::session_reader_loop(state); });
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
        auto lock = detail::acquire_session_lock(shard.mutex, detail::SessionLockKind::caller_table);
        // Close the check/register race with reader failure. If the reader has
        // already swept this shard, registering afterwards would leave a
        // stack Slot that no thread can ever complete.
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
    detail::session_trace_counters.request_id_probes.fetch_add(request_id_probes,
                                                               std::memory_order_relaxed);
#endif

    const detail::Deadline deadline = detail::deadline_from_now(state_->options.request_timeout);
    try {
        auto lock = detail::acquire_session_lock(state_->send_mutex, detail::SessionLockKind::send);
        detail::client::write_request_frame(state_->fd.get(), request_id, route, body,
                                            state_->options.io_timeout, deadline);
    } catch (...) {
        state_->broken.store(true, std::memory_order_release);
        {
            auto& shard = state_->shard_for(request_id);
            auto lock = detail::acquire_session_lock(shard.mutex,
                                                     detail::SessionLockKind::caller_table);
            if (auto* counters = state_->counters_for(request_id)) {
                ++counters->requests_failed;
            }
            shard.erase(request_id);
        }
        (void)::shutdown(state_->fd.get(), SHUT_RDWR);
        throw;
    }

    // The reader publishes the response before setting `done`; a short bounded
    // spin on the atomic flag avoids the futex wake round trip for responses
    // that land within tens of microseconds (the common high-frequency case).
    const detail::Deadline spin_deadline = detail::Clock::now() + session_spin_duration;
    while (!slot.done.load(std::memory_order_acquire)) {
        const detail::Deadline now = detail::Clock::now();
        if (now >= spin_deadline || now >= deadline) {
            break;
        }
        session_spin_hint();
    }

    bool timed_out = false;
    {
        auto slot_lock = detail::acquire_session_lock(slot.mutex, detail::SessionLockKind::waiter_slot);
        if (!slot.done.load(std::memory_order_acquire)) {
#ifdef EASY_UDS_TRACE_SPIN_MISS
            detail::session_spin_miss_count.fetch_add(1, std::memory_order_relaxed);
#endif
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
            detail::session_trace_counters.condition_waits.fetch_add(1, std::memory_order_relaxed);
#endif
            timed_out = !slot.cv.wait_until(slot_lock, deadline, [&slot] {
                return slot.done.load(std::memory_order_acquire);
            });
        }
    }
    {
        // Erasing under the table lock waits for any reader that already
        // resolved this pointer, so the stack slot cannot be destroyed early.
        auto& shard = state_->shard_for(request_id);
        auto lock = detail::acquire_session_lock(shard.mutex,
                                                 detail::SessionLockKind::caller_table);
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

Status Session::request_stream(std::string_view route, const StreamReader& request_body,
                               const std::function<void(std::string_view)>& response_chunk) {
    if (!state_) {
        throw std::logic_error("session has been moved from");
    }
    // Streams run on their own dedicated connection and are exclusive per
    // session (half-duplex), leaving the multiplexed session socket untouched.
    return detail::client::run_oneshot_stream(state_->socket_path, state_->options, route,
                                               request_body, response_chunk);
}

} // namespace easy_uds
