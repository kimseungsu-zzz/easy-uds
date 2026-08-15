#pragma once

#include "core.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

namespace easy_uds::detail {

// Byte source for worker-owned leases: buffered reactor bytes first, then
// blocking reads from the socket under the configured deadlines.
class StreamByteSource {
  public:
    StreamByteSource(std::string& buffered, std::size_t& offset, int fd)
        : buffered_(buffered), offset_(offset), fd_(fd) {}

    void read(void* data, std::size_t size, std::chrono::milliseconds io_timeout, Deadline deadline) {
        auto* bytes = static_cast<char*>(data);
        std::size_t received = 0;
        if (offset_ < buffered_.size()) {
            const std::size_t take = std::min(size, buffered_.size() - offset_);
            std::memcpy(bytes, buffered_.data() + offset_, take);
            offset_ += take;
            received += take;
        }
        while (received < size) {
            check_absolute_deadline(deadline, "receive timed out");
            const ssize_t result = socket_io::receive(fd_, bytes + received, size - received);
            if (result > 0) {
                received += static_cast<std::size_t>(result);
                continue;
            }
            if (result == 0) {
                throw_system_error("peer closed connection", ECONNRESET);
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wait_for_io(fd_, socket_wait::Interest::read, io_timeout, deadline,
                            "receive timed out");
                continue;
            }
            throw_system_error("receive failed");
        }
    }

    [[nodiscard]] bool buffered() const noexcept { return offset_ < buffered_.size(); }

  private:
    std::string& buffered_;
    std::size_t& offset_;
    int fd_;
};

} // namespace easy_uds::detail
