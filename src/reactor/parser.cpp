#include "parser.hpp"

#include "common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/socket.h>

namespace easy_uds::detail {

using protocol::WireType;

void consume(const std::shared_ptr<ServerState>& state,
             const std::shared_ptr<ReactorConnection>& rc) {
    const int fd = rc->conn->fd;
    std::array<char, reactor_read_scratch_size> scratch{};

    std::size_t read_ahead = 0;
    while (read_ahead < reactor_read_batch_size) {
        const std::size_t request_size =
            std::min(scratch.size(), reactor_read_batch_size - read_ahead);
        const ssize_t size = ::recv(fd, scratch.data(), request_size, 0);
        if (size > 0) {
            mark_io_progress(rc->conn);
            if (rc->pending_offset != 0 && rc->pending_offset == rc->pending.size()) {
                rc->pending.clear();
                rc->pending_offset = 0;
            }
            rc->pending.append(scratch.data(), static_cast<std::size_t>(size));
            read_ahead += static_cast<std::size_t>(size);
            continue;
        }
        if (size == 0) {
            rc->conn->closing.store(true, std::memory_order_release);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        rc->conn->closing.store(true, std::memory_order_release);
        return;
    }

    while (true) {
        const std::size_t available = rc->pending.size() - rc->pending_offset;
        if (rc->phase == ParsePhase::header) {
            if (available != 0 && rc->deadline == Deadline::max()) {
                rc->deadline = deadline_from_now(state->options.request_timeout);
            }
            const std::size_t need = protocol::header_size - rc->header_received;
            if (available < need) {
                return;
            }
            std::memcpy(rc->header.data() + rc->header_received,
                        rc->pending.data() + rc->pending_offset, need);
            rc->pending_offset += need;
            rc->header_received = 0;

            const auto decoded = protocol::decode_header(rc->header);
            if (decoded.type == WireType::request) {
                protocol::validate_request_lengths(decoded.arg1, decoded.arg2,
                                                   state->options.max_message_size);
                rc->request_id = decoded.request_id;
                rc->arg1 = decoded.arg1;
                rc->arg2 = decoded.arg2;
                rc->payload_total = static_cast<std::size_t>(decoded.arg1) + decoded.arg2;
                rc->payload_received = 0;
                rc->route_buffer.clear();
                rc->body_buffer.clear();
                rc->route_buffer.reserve(decoded.arg1);
                rc->body_buffer.reserve(decoded.arg2);
                rc->phase = ParsePhase::request_payload;
                continue;
            }
            if (decoded.type == WireType::stream_request) {
                if (decoded.arg1 == 0 || decoded.arg2 != 0 ||
                    decoded.arg1 > state->options.max_message_size) {
                    throw std::runtime_error("invalid stream request header");
                }
                rc->request_id = decoded.request_id;
                rc->arg1 = decoded.arg1;
                rc->route_buffer.clear();
                rc->route_buffer.reserve(decoded.arg1);
                rc->phase = ParsePhase::stream_route;
                rc->deadline = deadline_from_now(state->options.stream_timeout);
                continue;
            }
            throw std::runtime_error("unexpected protocol message type");
        }

        if (rc->phase == ParsePhase::request_payload) {
            const std::size_t remaining = rc->payload_total - rc->payload_received;
            const std::size_t take = std::min(available, remaining);
            if (take != 0) {
                const std::size_t route_part =
                    std::min(take, rc->arg1 - rc->route_buffer.size());
                rc->route_buffer.append(rc->pending.data() + rc->pending_offset, route_part);
                rc->body_buffer.append(rc->pending.data() + rc->pending_offset + route_part,
                                       take - route_part);
                rc->pending_offset += take;
                rc->payload_received += take;
            }
            if (rc->payload_received == rc->payload_total) {
                const bool read_paused = dispatch_request(state, rc);
                rc->phase = ParsePhase::header;
                rc->deadline = Deadline::max();
                if (read_paused) {
                    return;
                }
                continue;
            }
            return;
        }

        const std::size_t take =
            std::min(available, rc->arg1 - rc->route_buffer.size());
        rc->route_buffer.append(rc->pending.data() + rc->pending_offset, take);
        rc->pending_offset += take;
        if (rc->route_buffer.size() == rc->arg1) {
            dispatch_stream(state, rc);
        }
        return;
    }
}

} // namespace easy_uds::detail
