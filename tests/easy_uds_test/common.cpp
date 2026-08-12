#include "common.hpp"

namespace easy_uds::test {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test failed: " + message);
    }
}

std::string socket_path(const char* suffix) {
    return "/tmp/easy-uds-test-" + std::to_string(static_cast<long long>(::getpid())) + "-" + suffix + ".sock";
}

void wait_until_running(const easy_uds::Server& server) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!server.is_running()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("test failed: server did not enter running state");
        }
        std::this_thread::sleep_for(1ms);
    }
}

int connect_raw(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "raw socket failed");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        throw std::runtime_error("test socket path too long");
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "raw connect failed");
    }
    return fd;
}

ssize_t send_no_signal(int fd, const void* data, std::size_t size) {
#ifdef MSG_NOSIGNAL
    return ::send(fd, data, size, MSG_NOSIGNAL);
#else
    return ::send(fd, data, size, 0);
#endif
}

void send_exact(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t result = send_no_signal(fd, bytes + sent, size - sent);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(result == 0 ? EPIPE : errno, std::generic_category(), "raw send failed");
    }
}

void recv_exact(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < size) {
        const ssize_t result = ::recv(fd, bytes + received, size - received, 0);
        if (result > 0) {
            received += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        throw std::system_error(result == 0 ? ECONNRESET : errno, std::generic_category(),
                                "raw receive failed");
    }
}

void expect_peer_closed(int fd, std::chrono::milliseconds timeout, const std::string& message) {
    pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (result < 0 && errno == EINTR);
    if (result <= 0) {
        throw std::runtime_error("test failed: " + message);
    }
    unsigned char byte = 0;
    const ssize_t received = ::recv(fd, &byte, sizeof(byte), 0);
    if (received != 0) {
        throw std::runtime_error("test failed: " + message);
    }
}

std::uint32_t get_u32(const unsigned char* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | static_cast<std::uint32_t>(bytes[3]);
}

void cleanup_socket_artifacts(const std::string& path) {
    (void)::unlink(path.c_str());
    const std::string lock_path = path + ".lock";
    (void)::unlink(lock_path.c_str());
}

void make_stale_socket(const std::string& path) {
    (void)::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "stale socket creation failed");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "stale socket bind failed");
    }
    ::close(fd);
}

// Build a protocol-v2 frame.
std::vector<unsigned char> frame(std::uint8_t type, std::uint32_t request_id, std::uint32_t arg1,
                                 std::uint32_t arg2, const void* payload, std::size_t payload_size) {
    std::vector<unsigned char> bytes(20, 0);
    std::memcpy(bytes.data(), "EUDS", 4);
    bytes[4] = 2;
    bytes[5] = type;
    auto put = [&](std::size_t offset, std::uint32_t value) {
        bytes[offset + 0] = static_cast<unsigned char>(value >> 24);
        bytes[offset + 1] = static_cast<unsigned char>(value >> 16);
        bytes[offset + 2] = static_cast<unsigned char>(value >> 8);
        bytes[offset + 3] = static_cast<unsigned char>(value);
    };
    put(8, request_id);
    put(12, arg1);
    put(16, arg2);
    if (payload_size != 0) {
        const auto* bytes_payload = static_cast<const unsigned char*>(payload);
        bytes.insert(bytes.end(), bytes_payload, bytes_payload + payload_size);
    }
    return bytes;
}

std::vector<unsigned char> fixed_request(std::uint32_t request_id, const char* route, const char* body) {
    auto bytes = frame(1, request_id, static_cast<std::uint32_t>(std::strlen(route)),
                       static_cast<std::uint32_t>(std::strlen(body)), route, std::strlen(route));
    bytes.insert(bytes.end(), body, body + std::strlen(body));
    return bytes;
}

} // namespace easy_uds::test
