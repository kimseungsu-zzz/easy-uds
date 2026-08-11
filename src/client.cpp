#include "easy_uds/easy_uds.hpp"

#include "internal.hpp"

#include <algorithm>
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

Response read_response(BufferedReader& reader, std::size_t max_message_size, std::chrono::milliseconds io_timeout,
                       Deadline deadline) {
    HeaderBytes header{};
    reader.read(header.data(), header.size(), io_timeout, deadline);
    const auto decoded = protocol::decode_header(header, WireType::response);
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

Status Client::request_stream(std::string_view route, const StreamReader& request_body,
                              const std::function<void(std::string_view)>& response_chunk) const {
    return run_oneshot_stream(socket_path_, options_, route, request_body, response_chunk);
}

Session Client::session() const {
    return Session(socket_path_, options_);
}

// ---- Session ---------------------------------------------------------------

namespace detail {

struct SessionState {
    explicit SessionState(std::string socket_path, ClientOptions options)
        : socket_path(std::move(socket_path)), options(options) {}

    const std::string socket_path;
    ClientOptions options;

    FileDescriptor fd;
    std::mutex send_mutex;
    std::atomic<bool> broken{false};
    std::atomic<bool> reader_stop{false};

    // Single mutex + CV pair guards the in-flight table and serves every
    // pending request, so multiplexing costs one lock/unlock per response
    // instead of a per-slot mutex/CV round trip.
    std::mutex inflight_mutex;
    std::condition_variable inflight_cv;
    std::uint32_t next_id = 1;

    struct Slot {
        std::atomic<bool> done{false};
        Response response;
        std::exception_ptr error;
    };
    std::unordered_map<std::uint32_t, std::shared_ptr<Slot>> inflight;

    std::thread reader_thread;
};

void session_reader_loop(detail::SessionState* state) {
    try {
        BufferedReader reader(state->fd.get());
        while (!state->reader_stop.load(std::memory_order_relaxed)) {
            HeaderBytes header{};
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

            std::shared_ptr<detail::SessionState::Slot> slot;
            {
                std::lock_guard<std::mutex> lock(state->inflight_mutex);
                const auto it = state->inflight.find(decoded.request_id);
                if (it == state->inflight.end()) {
                    continue;  // timed out or already resolved: drop
                }
                slot = it->second;
                slot->response = std::move(response);
                slot->done.store(true, std::memory_order_release);
            }
            state->inflight_cv.notify_all();
        }
    } catch (...) {
        // Connection failed: break every pending request and mark the session
        // permanently unusable.
        state->broken.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state->inflight_mutex);
            for (auto& [id, slot] : state->inflight) {
                slot->error = std::current_exception();
                slot->done.store(true, std::memory_order_release);
            }
        }
        state->inflight_cv.notify_all();
    }
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
    if (!state_) {
        return;
    }
    state_->reader_stop.store(true, std::memory_order_relaxed);
    // shutdown() wakes the reader thread's blocked recv; join before close.
    if (state_->fd.get() >= 0) {
        (void)::shutdown(state_->fd.get(), SHUT_RDWR);
    }
    if (state_->reader_thread.joinable()) {
        state_->reader_thread.join();
    }
}

Session::Session(Session&& other) noexcept = default;
Session& Session::operator=(Session&& other) noexcept = default;

Response Session::request(std::string_view route, std::string_view body) {
    if (!state_) {
        throw std::logic_error("session has been moved from");
    }
    validate_request_lengths(route, body, state_->options.max_message_size);
    if (state_->broken.load(std::memory_order_acquire)) {
        throw std::logic_error("session connection is no longer usable");
    }

    auto slot = std::make_shared<detail::SessionState::Slot>();
    std::uint32_t request_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_->inflight_mutex);
        request_id = state_->next_id++;
        if (request_id == 0) {
            request_id = state_->next_id++;  // skip the reserved one-shot id
        }
        state_->inflight.emplace(request_id, slot);
    }

    const Deadline deadline = deadline_from_now(state_->options.request_timeout);
    try {
        std::lock_guard<std::mutex> lock(state_->send_mutex);
        write_request_frame(state_->fd.get(), request_id, route, body, state_->options.io_timeout, deadline);
    } catch (...) {
        state_->broken.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state_->inflight_mutex);
            state_->inflight.erase(request_id);
        }
        throw;
    }

    // The reader publishes the response before setting `done`; a short bounded
    // spin on the atomic flag avoids the futex wake round trip for responses
    // that land within tens of microseconds (the common high-frequency case).
    const Deadline spin_deadline = Clock::now() + std::chrono::microseconds{100};
    while (!slot->done.load(std::memory_order_acquire) && Clock::now() < spin_deadline &&
           Clock::now() < deadline) {
        std::this_thread::yield();
    }

    std::unique_lock<std::mutex> lock(state_->inflight_mutex);
    if (!slot->done.load(std::memory_order_acquire)) {
        if (!state_->inflight_cv.wait_until(lock, deadline, [&slot] { return slot->done.load(); })) {
            state_->inflight.erase(request_id);
            throw_system_error("request timed out", ETIMEDOUT);
        }
    }
    if (slot->error) {
        std::rethrow_exception(slot->error);
    }
    return slot->response;
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