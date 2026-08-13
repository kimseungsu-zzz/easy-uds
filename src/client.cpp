#include "easy_uds/easy_uds.hpp"

#include "internal.hpp"
#include "session_trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace easy_uds {
namespace {

using namespace detail;
using protocol::HeaderBytes;
using protocol::WireType;

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

void validate_request_lengths(std::string_view route, std::string_view body, std::size_t max_message_size) {
    protocol::validate_request_lengths(route.size(), body.size(), max_message_size);
}

void write_request_frame(int fd, std::uint32_t request_id, std::string_view route, std::string_view body,
                         std::chrono::milliseconds io_timeout, Deadline deadline) {
    const HeaderBytes header = protocol::encode_header(WireType::request, request_id,
                                                       static_cast<std::uint32_t>(route.size()),
                                                       static_cast<std::uint32_t>(body.size()));
    std::array<iovec, 3> parts{{
        {const_cast<unsigned char*>(header.data()), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact(fd, parts.data(), parts.size(), io_timeout, deadline);
}

// Like write_request_frame, but the frame header marks the request as carrying
// a descriptor and the descriptor rides as SCM_RIGHTS on the first sendmsg.
void write_request_frame_with_fd(int fd, std::uint32_t request_id, int passed_fd, std::string_view route,
                                 std::string_view body, std::chrono::milliseconds io_timeout,
                                 Deadline deadline) {
    const HeaderBytes header = protocol::encode_header(
        WireType::request, request_id, static_cast<std::uint32_t>(route.size()),
        static_cast<std::uint32_t>(body.size()), protocol::carries_fd_flag);
    std::array<iovec, 3> parts{{
        {const_cast<unsigned char*>(header.data()), header.size()},
        {const_cast<char*>(route.data()), route.size()},
        {const_cast<char*>(body.data()), body.size()},
    }};
    write_iovecs_exact_with_fd(fd, parts.data(), parts.size(), passed_fd, io_timeout, deadline);
}

Response read_response(BufferedReader& reader, std::size_t max_message_size, std::chrono::milliseconds io_timeout,
                       Deadline deadline) {
    HeaderBytes header{};
    reader.read(header.data(), header.size(), io_timeout, deadline);
    const auto decoded = protocol::decode_header(header, WireType::response);
    if (decoded.request_id != 0) {
        throw std::runtime_error("unexpected response request_id");
    }
    if (decoded.arg1 > static_cast<std::uint32_t>(INT32_MAX)) {
        throw std::runtime_error("response status_code is out of range");
    }
    if (decoded.arg2 > max_message_size) {
        throw std::length_error("response exceeds max_message_size");
    }
    Response response;
    response.status = static_cast<Status>(decoded.arg1);
    response.body.resize(decoded.arg2);
    reader.read(response.body.data(), response.body.size(), io_timeout, deadline);
    return response;
}

void write_stream_request(int fd, std::uint32_t request_id, std::string_view route, const StreamReader& body,
                          std::size_t chunk_size, std::size_t max_stream_size, std::chrono::milliseconds io_timeout,
                          Deadline deadline) {
    write_frame_with_payload(fd, WireType::stream_request, request_id, static_cast<std::uint32_t>(route.size()), 0,
                             route.data(), route.size(), io_timeout, deadline);

    std::vector<char> buffer(chunk_size);
    std::size_t total_size = 0;
    while (body) {
        const std::size_t size = body(buffer.data(), buffer.size());
        if (size == 0) {
            break;
        }
        if (size > buffer.size()) {
            throw std::runtime_error("stream reader returned more bytes than its capacity");
        }
        if (size > std::numeric_limits<std::size_t>::max() - total_size ||
            (max_stream_size != 0 && size > max_stream_size - total_size)) {
            throw std::length_error("stream exceeds max_stream_size");
        }
        total_size += size;
        write_frame_with_payload(fd, WireType::stream_request_chunk, request_id,
                                 static_cast<std::uint32_t>(size), 0, buffer.data(), size, io_timeout, deadline);
    }
    write_header_frame(fd, WireType::stream_request_end, request_id, 0, 0, io_timeout, deadline);
}

// One-shot streamed exchange on a dedicated connection (used by
// Client::request_stream and Session::request_stream).
Status run_oneshot_stream(const std::string& socket_path, const ClientOptions& options, std::string_view route,
                          const StreamReader& request_body,
                          const std::function<void(std::string_view)>& response_chunk) {
    if (route.empty()) {
        throw std::invalid_argument("route must not be empty");
    }
    if (route.size() > options.max_message_size || route.size() > protocol::max_wire_field) {
        throw std::length_error("route exceeds max_message_size");
    }

    const Deadline deadline = deadline_from_now(options.stream_timeout);
    FileDescriptor fd = make_socket();
    const sockaddr_un address = make_address(socket_path);
    connect_nonblocking(fd.get(), address, options.connect_timeout, deadline);

    write_stream_request(fd.get(), 0, route, request_body, options.stream_chunk_size, options.max_stream_size,
                         options.io_timeout, deadline);

    BufferedReader reader(fd.get());
    HeaderBytes header{};
    reader.read(header.data(), header.size(), options.io_timeout, deadline);
    const auto decoded = protocol::decode_header(header, WireType::stream_response);
    if (decoded.request_id != 0 || decoded.arg1 > static_cast<std::uint32_t>(INT32_MAX) || decoded.arg2 != 0) {
        throw std::runtime_error("invalid stream response header");
    }

    std::vector<char> buffer(options.stream_chunk_size);
    std::size_t total_size = 0;
    while (true) {
        HeaderBytes chunk_header{};
        reader.read(chunk_header.data(), chunk_header.size(), options.io_timeout, deadline);
        const auto chunk = protocol::decode_header(chunk_header);
        if (chunk.type == WireType::stream_response_end) {
            if (chunk.request_id != 0 || chunk.arg1 != 0 || chunk.arg2 != 0) {
                throw std::runtime_error("invalid stream end frame");
            }
            break;
        }
        if (chunk.type != WireType::stream_response_chunk || chunk.request_id != 0 || chunk.arg1 == 0 ||
            chunk.arg2 != 0) {
            throw std::runtime_error("invalid stream chunk frame");
        }
        const std::size_t chunk_size = chunk.arg1;
        if (chunk_size > std::numeric_limits<std::size_t>::max() - total_size ||
            (options.max_stream_size != 0 && chunk_size > options.max_stream_size - total_size)) {
            throw std::length_error("stream exceeds max_stream_size");
        }
        total_size += chunk_size;
        std::size_t remaining = chunk.arg1;
        while (remaining != 0) {
            const std::size_t take = std::min(remaining, buffer.size());
            reader.read(buffer.data(), take, options.io_timeout, deadline);
            if (response_chunk) {
                response_chunk(std::string_view(buffer.data(), take));
            }
            remaining -= take;
        }
    }
    return static_cast<Status>(decoded.arg1);
}

} // namespace

// ---- Client ----------------------------------------------------------------

Client::Client(std::string socket_path, ClientOptions options)
    : socket_path_(std::move(socket_path)), options_(options) {
    (void)make_address(socket_path_);
    validate_client_options(options_);
}

Response Client::request(std::string_view route, std::string_view body) const {
    validate_request_lengths(route, body, options_.max_message_size);

    const Deadline deadline = deadline_from_now(options_.request_timeout);
    FileDescriptor fd = make_socket();
    const sockaddr_un address = make_address(socket_path_);
    connect_nonblocking(fd.get(), address, options_.connect_timeout, deadline);

    write_request_frame(fd.get(), 0, route, body, options_.io_timeout, deadline);
    BufferedReader reader(fd.get());
    return read_response(reader, options_.max_message_size, options_.io_timeout, deadline);
}

Response Client::request_fd(std::string_view route, int fd, std::string_view body) const {
    if (fd < 0) {
        throw std::invalid_argument("request_fd requires a valid descriptor");
    }
    validate_request_lengths(route, body, options_.max_message_size);

    const Deadline deadline = deadline_from_now(options_.request_timeout);
    FileDescriptor socket_fd = make_socket();
    const sockaddr_un address = make_address(socket_path_);
    connect_nonblocking(socket_fd.get(), address, options_.connect_timeout, deadline);

    write_request_frame_with_fd(socket_fd.get(), 0, fd, route, body, options_.io_timeout, deadline);
    BufferedReader reader(socket_fd.get());
    return read_response(reader, options_.max_message_size, options_.io_timeout, deadline);
}

Status Client::request_stream(std::string_view route, const StreamReader& request_body,
                              const std::function<void(std::string_view)>& response_chunk) const {
    return run_oneshot_stream(socket_path_, options_, route, request_body, response_chunk);
}

Session Client::session() const {
    return Session(socket_path_, options_);
}

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
    explicit SessionState(std::string socket_path, ClientOptions options)
        : socket_path(std::move(socket_path)), options(options) {}

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
    static constexpr std::size_t inflight_shard_count = EASY_UDS_SESSION_INFLIGHT_SHARDS;
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

    [[nodiscard]] InflightShard& shard_for(std::uint32_t request_id) noexcept {
        return inflight_shards[request_id % inflight_shard_count];
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
                throw std::runtime_error("response status_code is out of range");
            }
            if (decoded.arg2 > state->options.max_message_size) {
                throw std::length_error("response exceeds max_message_size");
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
                    throw std::runtime_error("unexpected response request_id");
                }
                auto* const slot = it->second;
                auto slot_lock = acquire_session_lock(slot->mutex, SessionLockKind::reader_slot);
                if (slot->done.load(std::memory_order_acquire)) {
                    throw std::runtime_error("duplicate response request_id");
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
    const Deadline deadline = deadline_from_now(state_->options.request_timeout);
    state_->fd = make_socket();
    const sockaddr_un address = make_address(state_->socket_path);
    connect_nonblocking(state_->fd.get(), address, state_->options.connect_timeout, deadline);
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

Response Session::request(std::string_view route, std::string_view body) {
    if (!state_) {
        throw std::logic_error("session has been moved from");
    }
    validate_request_lengths(route, body, state_->options.max_message_size);
    if (state_->broken.load(std::memory_order_acquire)) {
        throw std::logic_error("session connection is no longer usable");
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
            throw std::logic_error("session connection is no longer usable");
        }
        if (shard.inflight.find(request_id) != shard.inflight.end()) {
            continue;
        }
        shard.insert(request_id, &slot);
        break;
    }
#ifdef EASY_UDS_TRACE_SESSION_CONTENTION
    detail::session_trace_counters.request_id_probes.fetch_add(request_id_probes,
                                                               std::memory_order_relaxed);
#endif

    const Deadline deadline = deadline_from_now(state_->options.request_timeout);
    try {
        auto lock = detail::acquire_session_lock(state_->send_mutex, detail::SessionLockKind::send);
        write_request_frame(state_->fd.get(), request_id, route, body, state_->options.io_timeout, deadline);
    } catch (...) {
        state_->broken.store(true, std::memory_order_release);
        {
            auto& shard = state_->shard_for(request_id);
            auto lock = detail::acquire_session_lock(shard.mutex,
                                                     detail::SessionLockKind::caller_table);
            shard.erase(request_id);
        }
        (void)::shutdown(state_->fd.get(), SHUT_RDWR);
        throw;
    }

    // The reader publishes the response before setting `done`; a short bounded
    // spin on the atomic flag avoids the futex wake round trip for responses
    // that land within tens of microseconds (the common high-frequency case).
    const Deadline spin_deadline = Clock::now() + session_spin_duration;
    while (!slot.done.load(std::memory_order_acquire)) {
        const Deadline now = Clock::now();
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
        shard.erase(request_id);
    }
    if (timed_out) {
        state_->broken.store(true, std::memory_order_release);
        (void)::shutdown(state_->fd.get(), SHUT_RDWR);
        throw_system_error("request timed out", ETIMEDOUT);
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
    return run_oneshot_stream(state_->socket_path, state_->options, route, request_body, response_chunk);
}

} // namespace easy_uds
