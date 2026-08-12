#include "core.hpp"
#include "stream_io.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace easy_uds::detail {
namespace {

using protocol::HeaderBytes;
using protocol::WireType;

easy_uds::StreamReader bounded_error_body_reader(std::string_view message, std::size_t max_message_size) {
    std::string body = message.size() <= max_message_size ? std::string(message) : std::string{};
    return [body = std::move(body), offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
        if (offset >= body.size()) {
            return std::size_t{0};
        }
        const std::size_t take = std::min(capacity, body.size() - offset);
        std::memcpy(output, body.data() + offset, take);
        offset += take;
        return take;
    };
}

// Incremental stream frame reader. Chunk/end frames must carry the stream's
// request id; anything else is a protocol error.
class IncomingStream {
  public:
    IncomingStream(StreamByteSource& source, WireType chunk_type, WireType end_type, std::size_t max_size,
                   std::chrono::milliseconds io_timeout, Deadline deadline)
        : source_(source), chunk_type_(chunk_type), end_type_(end_type), max_size_(max_size),
          io_timeout_(io_timeout), deadline_(deadline) {}

    std::size_t read(char* buffer, std::size_t capacity) {
        if (ended_) {
            return 0;
        }
        if (frame_remaining_ == 0) {
            HeaderBytes header{};
            source_.read(header.data(), header.size(), io_timeout_, deadline_);
            const auto decoded = protocol::decode_header(header);
            if (decoded.type == end_type_) {
                if (decoded.request_id != request_id_ || decoded.arg1 != 0 || decoded.arg2 != 0) {
                    throw std::runtime_error("invalid stream end frame");
                }
                ended_ = true;
                return 0;
            }
            if (decoded.type != chunk_type_ || decoded.request_id != request_id_ || decoded.arg1 == 0 ||
                decoded.arg2 != 0) {
                throw std::runtime_error("invalid stream chunk frame");
            }
            const std::size_t chunk_size = decoded.arg1;
            if (chunk_size > std::numeric_limits<std::size_t>::max() - total_size_ ||
                (max_size_ != 0 && chunk_size > max_size_ - total_size_)) {
                throw std::length_error("stream exceeds max_stream_size");
            }
            total_size_ += chunk_size;
            frame_remaining_ = chunk_size;
        }

        const std::size_t size = std::min(capacity, frame_remaining_);
        source_.read(buffer, size, io_timeout_, deadline_);
        frame_remaining_ -= size;
        return size;
    }

    void drain(std::size_t buffer_size) {
        std::vector<char> buffer(buffer_size);
        while (read(buffer.data(), buffer.size()) != 0) {
        }
    }

    void set_request_id(std::uint32_t request_id) noexcept { request_id_ = request_id; }

  private:
    StreamByteSource& source_;
    WireType chunk_type_;
    WireType end_type_;
    std::size_t max_size_;
    std::chrono::milliseconds io_timeout_;
    Deadline deadline_;
    std::size_t total_size_ = 0;
    std::size_t frame_remaining_ = 0;
    std::uint32_t request_id_ = 0;
    bool ended_ = false;
};

class ActiveStreamGuard {
  public:
    explicit ActiveStreamGuard(std::shared_ptr<ServerState> state) : state_(std::move(state)) {}
    ~ActiveStreamGuard() {
        if (state_) {
            state_->active_streams.fetch_sub(1, std::memory_order_relaxed);
        }
    }
    ActiveStreamGuard(const ActiveStreamGuard&) = delete;
    ActiveStreamGuard& operator=(const ActiveStreamGuard&) = delete;

  private:
    std::shared_ptr<ServerState> state_;
};

} // namespace

void run_stream_exchange(const std::shared_ptr<ServerState>& state, PendingJob&& job) {
    auto connection = job.connection;
    const int fd = connection->fd;
    ActiveStreamGuard stream_guard(state);
    try {
        StreamByteSource source(job.buffered, job.buffered_offset, fd);
        IncomingStream incoming(source, WireType::stream_request_chunk, WireType::stream_request_end,
                                state->options.max_stream_size, state->options.io_timeout, job.deadline);
        incoming.set_request_id(job.request.request_id);
        StreamReader body_reader = [&incoming](char* buffer, std::size_t capacity) {
            return incoming.read(buffer, capacity);
        };

        StreamResponse response;
        std::shared_ptr<const StreamHandlerEntry> handler;
        if (!find_stream_handler(state, job.request.route, handler)) {
            response.status = 404;
        } else {
            try {
                response = handler->handler(body_reader, job.request);
            } catch (const std::exception& error) {
                response = state->options.include_handler_error_messages
                               ? StreamResponse{500, bounded_error_body_reader(error.what(),
                                                                               state->options.max_message_size)}
                               : StreamResponse{500, {}};
            } catch (...) {
                response = {500, {}};
            }
        }

        incoming.drain(state->options.stream_chunk_size);
        if (response.status < 0) {
            response = {500, {}};
        }
        {
            std::lock_guard<std::mutex> lock(connection->write_mutex);
            write_header_frame(fd, WireType::stream_response, job.request.request_id,
                               static_cast<std::uint32_t>(response.status), 0, state->options.io_timeout,
                               job.deadline);
            std::vector<char> chunk_buffer(state->options.stream_chunk_size);
            std::size_t total_size = 0;
            while (response.body) {
                const std::size_t size = response.body(chunk_buffer.data(), chunk_buffer.size());
                if (size == 0) {
                    break;
                }
                if (size > chunk_buffer.size() || size > std::numeric_limits<std::size_t>::max() - total_size ||
                    (state->options.max_stream_size != 0 && size > state->options.max_stream_size - total_size)) {
                    throw std::length_error("stream exceeds max_stream_size");
                }
                total_size += size;
                write_frame_with_payload(fd, WireType::stream_response_chunk, job.request.request_id,
                                         static_cast<std::uint32_t>(size), 0, chunk_buffer.data(), size,
                                         state->options.io_timeout, job.deadline);
            }
            write_header_frame(fd, WireType::stream_response_end, job.request.request_id, 0, 0,
                               state->options.io_timeout, job.deadline);
        }
    } catch (...) {
        connection->closing.store(true, std::memory_order_release);
    }

    rearm_connection(state, connection, std::move(job.buffered), job.buffered_offset);
}

} // namespace easy_uds::detail
