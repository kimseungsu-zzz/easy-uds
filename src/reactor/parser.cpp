#include "parser.hpp"

#include "common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <sys/socket.h>
#include <unistd.h>

namespace easy_uds::detail {
namespace {

using protocol::WireType;

// recvmsg with a one-descriptor control buffer. A request frame carries at
// most one descriptor; truncation or malformed ancillary data is fatal rather
// than silently dropping a descriptor and desynchronising later frames.
ssize_t recv_with_fds(int fd, char* data, std::size_t size, std::deque<int>& fds) {
    iovec vector{data, size};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    char control[CMSG_SPACE(sizeof(int))]{};
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    int receive_flags = 0;
#ifdef MSG_CMSG_CLOEXEC
    receive_flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t result = ::recvmsg(fd, &message, receive_flags);
    if (result > 0) {
        int captured_fd = -1;
        bool malformed = false;
        for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
             header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
                continue;
            }
            if (header->cmsg_len != CMSG_LEN(sizeof(int)) || captured_fd >= 0) {
                malformed = true;
                continue;
            }
            std::memcpy(&captured_fd, CMSG_DATA(header), sizeof(captured_fd));
        }
        if ((message.msg_flags & MSG_CTRUNC) != 0 || malformed) {
            if (captured_fd >= 0) {
                (void)::close(captured_fd);
            }
            throw std::runtime_error("invalid or truncated ancillary descriptor");
        }
        if (captured_fd >= 0) {
#ifndef MSG_CMSG_CLOEXEC
            try {
                set_close_on_exec(captured_fd);
            } catch (...) {
                (void)::close(captured_fd);
                throw;
            }
#endif
            fds.push_back(captured_fd);
        }
    }
    return result;
}

} // namespace

bool begin_request_payload(const std::shared_ptr<ServerState>& state,
                           const std::shared_ptr<ReactorConnection>& rc) {
    const std::size_t request_bytes = rc->payload_total;
    if (state->options.max_total_inflight_bytes != 0) {
        if (!try_reserve_connection_request_bytes(state, rc->conn, request_bytes)) {
            pause_connection_reads(state, rc);
            return false;
        }
        rc->reserved_request_bytes = request_bytes;
    }
    rc->route_buffer.clear();
    rc->body_buffer.clear();
    rc->route_buffer.reserve(rc->arg1);
    rc->body_buffer.reserve(rc->arg2);
    rc->phase = ParsePhase::request_payload;
    return true;
}

bool begin_stream_route(const std::shared_ptr<ServerState>& state,
                        const std::shared_ptr<ReactorConnection>& rc) {
    const std::size_t request_bytes = rc->arg1;
    if (state->options.max_total_inflight_bytes != 0) {
        if (!try_reserve_connection_request_bytes(state, rc->conn, request_bytes)) {
            pause_connection_reads(state, rc);
            return false;
        }
        rc->reserved_request_bytes = request_bytes;
    }
    rc->route_buffer.clear();
    rc->route_buffer.reserve(rc->arg1);
    rc->phase = ParsePhase::stream_route;
    return true;
}

void release_consumed_pending(const std::shared_ptr<ReactorConnection>& rc,
                              bool strict_budget) {
    if (strict_budget && rc->pending_offset == rc->pending.size()) {
        std::string{}.swap(rc->pending);
        rc->pending_offset = 0;
    }
}

void consume(const std::shared_ptr<ServerState>& state,
             const std::shared_ptr<ReactorConnection>& rc) {
    const int fd = rc->conn->fd;
    std::array<char, reactor_read_scratch_size> scratch{};
    const bool strict_budget = state->options.max_total_inflight_bytes != 0;

    if (rc->phase == ParsePhase::request_budget &&
        !begin_request_payload(state, rc)) {
        return;
    }
    if (rc->phase == ParsePhase::stream_budget &&
        !begin_stream_route(state, rc)) {
        return;
    }

    std::size_t read_ahead = 0;
    while (read_ahead < reactor_read_batch_size) {
        std::size_t request_size =
            std::min(scratch.size(), reactor_read_batch_size - read_ahead);
        if (strict_budget) {
            const std::size_t available = rc->pending.size() - rc->pending_offset;
            if (rc->phase == ParsePhase::header) {
                const std::size_t need = protocol::header_size - rc->header_received;
                request_size = std::min(request_size, need > available ? need - available : 0);
            } else if (rc->phase == ParsePhase::request_payload) {
                const std::size_t remaining = rc->payload_total - rc->payload_received;
                request_size = std::min(request_size, remaining > available ? remaining - available : 0);
            } else if (rc->phase == ParsePhase::stream_route) {
                const std::size_t remaining = rc->arg1 - rc->route_buffer.size();
                request_size = std::min(request_size, remaining > available ? remaining - available : 0);
            }
            if (request_size == 0) {
                break;
            }
        }
        const ssize_t size = recv_with_fds(fd, scratch.data(), request_size, rc->received_fds);
        if (size > 0) {
            mark_io_progress(rc->conn);
            if (rc->pending_offset != 0 && rc->pending_offset == rc->pending.size()) {
                rc->pending.clear();
                rc->pending_offset = 0;
            }
            rc->pending.append(scratch.data(), static_cast<std::size_t>(size));
            read_ahead += static_cast<std::size_t>(size);
            if (strict_budget) {
                // Parse the bounded chunk before reading again. In particular,
                // never read payload bytes until its declared allocation has
                // been admitted by the aggregate budget.
                break;
            }
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
            const std::size_t take = std::min(available, need);
            if (take != 0) {
                std::memcpy(rc->header.data() + rc->header_received,
                            rc->pending.data() + rc->pending_offset, take);
                rc->pending_offset += take;
                rc->header_received += take;
            }
            if (rc->header_received != protocol::header_size) {
                release_consumed_pending(rc, strict_budget);
                return;
            }
            rc->header_received = 0;
            release_consumed_pending(rc, strict_budget);

            const auto decoded = protocol::decode_header(rc->header);
            if (decoded.type == WireType::request) {
                if (decoded.flags & protocol::carries_fd_flag) {
                    if (rc->received_fds.empty()) {
                        throw std::runtime_error("fd frame without ancillary descriptor");
                    }
                    rc->request_fd = rc->received_fds.front();
                    rc->received_fds.pop_front();
                }
                protocol::validate_request_lengths(decoded.arg1, decoded.arg2,
                                                   state->options.max_message_size);
                rc->request_id = decoded.request_id;
                rc->arg1 = decoded.arg1;
                rc->arg2 = decoded.arg2;
                rc->payload_total = static_cast<std::size_t>(decoded.arg1) + decoded.arg2;
                rc->payload_received = 0;
                rc->phase = ParsePhase::request_budget;
                if (!begin_request_payload(state, rc)) {
                    return;
                }
                continue;
            }
            if (decoded.type == WireType::stream_request) {
                if (decoded.flags & protocol::carries_fd_flag) {
                    throw std::runtime_error("fd flag is only supported on fixed requests");
                }
                if (decoded.arg1 == 0 || decoded.arg2 != 0 ||
                    decoded.arg1 > state->options.max_message_size) {
                    throw std::runtime_error("invalid stream request header");
                }
                rc->request_id = decoded.request_id;
                rc->arg1 = decoded.arg1;
                rc->arg2 = 0;
                rc->phase = ParsePhase::stream_budget;
                rc->deadline = deadline_from_now(state->options.stream_timeout);
                if (!begin_stream_route(state, rc)) {
                    return;
                }
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
            release_consumed_pending(rc, strict_budget);
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
        release_consumed_pending(rc, strict_budget);
        if (rc->route_buffer.size() == rc->arg1) {
            dispatch_stream(state, rc);
        }
        return;
    }
}

} // namespace easy_uds::detail
