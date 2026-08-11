// Stateful session fuzzer: feeds adversarial byte sequences into a live
// server over a real AF_UNIX connection, exercising the persistent-connection
// session loop, read-ahead buffering, and stream frame parsing. The server
// runs in-process, so any crash (checked by ASan/UBSan in CI) fails the run.
#include "easy_uds/easy_uds.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr const char* kSocketPath = "/tmp/easy-uds-session-fuzz.sock";

struct FuzzServer {
    easy_uds::Server server;
    std::thread thread;

    FuzzServer()
        : server(kSocketPath, make_options()) {
        server.on("ping", [](const easy_uds::Request&) { return easy_uds::Response{200, "pong"}; });
        server.on("echo", [](const easy_uds::Request& request) { return easy_uds::Response{200, request.body}; });
        server.on_stream("stream", [](const easy_uds::StreamReader& body, const easy_uds::Request&) {
            std::array<char, 256> buffer{};
            while (body(buffer.data(), buffer.size()) != 0) {
            }
            return easy_uds::StreamResponse{200, {}};
        });
        thread = std::thread([this] { server.run(); });
        while (!server.is_running()) {
            std::this_thread::yield();
        }
    }

    ~FuzzServer() {
        server.stop();
        thread.join();
        (void)::unlink(kSocketPath);
    }

    static easy_uds::ServerOptions make_options() {
        easy_uds::ServerOptions options;
        options.worker_threads = 2;
        options.max_connections = 16;
        options.max_message_size = 1024U * 1024U;
        options.io_timeout = std::chrono::milliseconds{200};
        options.request_timeout = std::chrono::milliseconds{500};
        options.stream_timeout = std::chrono::milliseconds{200};
        options.stale_socket_grace_period = std::chrono::milliseconds{0};
        return options;
    }
};

FuzzServer& fuzz_server() {
    static FuzzServer instance;
    return instance;
}

int connect_socket() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, kSocketPath, std::strlen(kSocketPath) + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    constexpr std::size_t kMaxInput = 64U * 1024U;
    if (size == 0 || size > kMaxInput) {
        return 0;
    }

    (void)fuzz_server();

    const int fd = connect_socket();
    if (fd < 0) {
        return 0;
    }

    if (::write(fd, data, size) != static_cast<ssize_t>(size)) {
        ::close(fd);
        return 0;
    }

    // Drain whatever the server replies for a bounded window, then close. The
    // server closes the connection on malformed frames; readers must never hang.
    std::array<char, 4096> buffer{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd item{};
        item.fd = fd;
        item.events = POLLIN;
        const int result = ::poll(&item, 1, 5);
        if (result <= 0) {
            break;
        }
        const ssize_t read_result = ::read(fd, buffer.data(), buffer.size());
        if (read_result <= 0) {
            break;
        }
    }

    ::close(fd);
    return 0;
}