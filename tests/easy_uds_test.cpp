#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <poll.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error("test failed: " + message);
    }
}

template <typename Exception, typename Function>
void expect_throws(Function&& function, const std::string& message) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error("test failed: " + message);
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

std::vector<unsigned char> fixed_request(std::uint32_t request_id, const char* route, const char* body = "") {
    auto bytes = frame(1, request_id, static_cast<std::uint32_t>(std::strlen(route)),
                       static_cast<std::uint32_t>(std::strlen(body)), route, std::strlen(route));
    bytes.insert(bytes.end(), body, body + std::strlen(body));
    return bytes;
}

void test_option_validation() {
    using namespace easy_uds;

    ServerOptions server_options;
    server_options.worker_threads = 0;
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-workers"), server_options); }, "zero worker_threads should be rejected");

    server_options = {};
    server_options.worker_threads = 3;
    server_options.max_connections = 2;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-maxconn"), server_options); },
                                         "worker_threads beyond max_connections should be rejected");

    server_options = {};
    server_options.max_concurrent_streams = 99;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-streams"), server_options); },
                                         "max_concurrent_streams beyond worker_threads should be rejected");

    server_options = {};
    server_options.max_message_size = 0;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-msg"), server_options); },
                                         "zero max_message_size should be rejected");

    server_options = {};
    server_options.socket_permissions = 01000;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-perms"), server_options); },
                                         "socket_permissions beyond 0777 should be rejected");

    ClientOptions client_options;
    client_options.connect_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-client"), client_options); },
                                         "negative connect_timeout should be rejected");

    client_options = {};
    client_options.stream_chunk_size = 0;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-client2"), client_options); },
                                         "zero stream_chunk_size should be rejected");
}

void test_socket_path_safety() {
    using namespace easy_uds;

    expect_throws<std::invalid_argument>([&] { Server server(""); }, "empty socket path should be rejected");
    expect_throws<std::invalid_argument>([&] { Server server(std::string("bad\0nul", 7)); },
                                         "embedded NUL should be rejected");
    expect_throws<std::invalid_argument>([&] { Server server(std::string(200, 'x')); },
                                         "overlong socket path should be rejected");

    const std::string path = socket_path("path-safety");
    ServerOptions options;
    options.stale_socket_grace_period = 0ms;
    Server server(path, options);

    struct stat info {};
    expect(::lstat(path.c_str(), &info) == 0, "socket file should exist after construction");
    expect((info.st_mode & 0777U) == 0600U, "socket permissions should honor ServerOptions");

    expect_throws<std::runtime_error>([&] { Server duplicate(path); }, "active socket path should not be replaced");

    std::exception_ptr run_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            run_error = std::current_exception();
        }
    });
    wait_until_running(server);
    server.stop();
    server_thread.join();
    if (run_error) {
        std::rethrow_exception(run_error);
    }
    cleanup_socket_artifacts(path);
}

void test_stop_preserves_replaced_socket_path() {
    using namespace easy_uds;

    const std::string path = socket_path("replacement-safety");
    ServerOptions options;
    options.stale_socket_grace_period = 0ms;
    Server server(path, options);

    expect(::unlink(path.c_str()) == 0, "original server socket should be removable for replacement test");
    const int replacement = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (replacement < 0) {
        throw std::system_error(errno, std::generic_category(), "replacement socket creation failed");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(replacement, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = errno;
        ::close(replacement);
        throw std::system_error(error, std::generic_category(), "replacement socket bind failed");
    }

    server.stop();
    struct stat replacement_info {};
    expect(::lstat(path.c_str(), &replacement_info) == 0 && S_ISSOCK(replacement_info.st_mode),
           "stop must not unlink a socket that replaced the server-owned inode");
    ::close(replacement);
    cleanup_socket_artifacts(path);
}

void test_stale_socket_cleanup() {
    using namespace easy_uds;

    const std::string path = socket_path("stale");
    make_stale_socket(path);
    ServerOptions options;
    options.stale_socket_grace_period = 0ms;
    Server server(path, options);
    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    const Response response = client.request("missing");  // route absent, but server must be alive
    expect(response.status == 404, "stale socket should be replaced and the server should respond");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_basic_server() {
    using namespace easy_uds;

    const std::string path = socket_path("basic");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 32;
    options.max_message_size = 256;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on("echo", [](const Request& request) { return Response{200, request.body}; });
    server.on(std::string("route\nwith\0binary", 17), [](const Request& request) { return Response{201, request.body}; });
    server.on("throws", [](const Request&) -> Response { throw std::runtime_error("boom"); });
    server.on("too-large", [](const Request&) { return Response{200, std::string(300, 'x')}; });
    server.on("bad-status", [](const Request&) { return Response{-1, "bad"}; });
    server.on_prefix("titan.", [](const Request& request) { return Response{200, "prefix:" + request.route}; });
    server.on_prefix("titan.robot.", [](const Request& request) { return Response{200, "deep:" + request.route}; });

    expect_throws<std::runtime_error>([&] { server.on("ping", [](const Request&) { return Response{}; }); },
                                      "duplicate route should be rejected");
    expect_throws<std::runtime_error>([&] { server.on_prefix("titan.", [](const Request&) { return Response{}; }); },
                                      "duplicate prefix route should be rejected");
    expect_throws<std::invalid_argument>([&] { server.on("", [](const Request&) { return Response{}; }); },
                                         "empty server route should be rejected");

    struct stat info {};
    expect(::lstat(path.c_str(), &info) == 0, "socket file should exist after construction");

    std::exception_ptr run_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            run_error = std::current_exception();
        }
    });
    wait_until_running(server);

    // Routes may be registered while running.
    server.on("late", [](const Request&) { return Response{200, "late"}; });

    ClientOptions client_options;
    client_options.max_message_size = 256;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 500ms;
    Client client(path, client_options);

    Response response = client.request("ping");
    expect(response.status == 200 && response.body == "pong", "ping response");

    const std::string binary_body("hello\0world\nagain", 17);
    response = client.request("echo", binary_body);
    expect(response.status == 200 && response.body == binary_body, "binary body should round-trip exactly");

    const std::string binary_route("route\nwith\0binary", 17);
    response = client.request(binary_route, "ok");
    expect(response.status == 201 && response.body == "ok", "binary route should be supported by the wire protocol");

    response = client.request("missing");
    expect(response.status == 404 && response.body == "Not Found", "missing route response");

    response = client.request("throws");
    expect(response.status == 500 && response.body == "boom", "throwing handler response should carry its message");

    response = client.request("too-large");
    expect(response.status == 500 && response.body == "response exceeds max_message_size",
           "oversized handler response should carry its message");

    response = client.request("bad-status");
    expect(response.status == 500 && response.body == "response status_code must not be negative",
           "invalid handler status response should carry its message");

    response = client.request("late");
    expect(response.status == 200 && response.body == "late", "late-registered route should work");

    response = client.request("titan.1.move");
    expect(response.status == 200 && response.body == "prefix:titan.1.move", "prefix route should match");
    response = client.request("titan.robot.2.turn");
    expect(response.status == 200 && response.body == "deep:titan.robot.2.turn",
           "longest prefix should win");
    expect_throws<std::invalid_argument>([&] { (void)client.request(""); },
                                         "empty client route should be rejected");

    std::array<unsigned char, 20> v1_header{};
    std::memcpy(v1_header.data(), "EUDS", 4);
    v1_header[4] = 1;  // protocol v1 must be rejected by a v2 server
    v1_header[5] = 1;
    const int malformed_fd = connect_raw(path);
    expect(send_no_signal(malformed_fd, v1_header.data(), v1_header.size()) == static_cast<ssize_t>(v1_header.size()),
           "v1 header should be written");
    ::close(malformed_fd);
    std::this_thread::sleep_for(10ms);
    expect(client.request("ping").body == "pong", "malformed client must not kill the server");

    server.stop();
    server_thread.join();
    if (run_error) {
        std::rethrow_exception(run_error);
    }
    cleanup_socket_artifacts(path);
}

void test_peer_credentials() {
    using namespace easy_uds;

    const std::string path = socket_path("peer");
    ServerOptions options;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("who", [](const Request& request) {
        if (!request.peer.present) {
            return Response{500, "no-peer"};
        }
        return Response{200, std::to_string(request.peer.pid) + ":" + std::to_string(request.peer.uid) + ":" +
                                std::to_string(request.peer.gid)};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    const Response response = client.request("who");
    expect(response.status == 200, "peer credentials should be available");
    const std::string expected = std::to_string(static_cast<long long>(::getpid())) + ":" +
                                 std::to_string(static_cast<long long>(::geteuid())) + ":" +
                                 std::to_string(static_cast<long long>(::getegid()));
    expect(response.body == expected, "peer pid/uid/gid should match the connecting process");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_tiny_message_limit_404() {
    using namespace easy_uds;

    const std::string path = socket_path("tiny-404");
    ServerOptions options;
    options.max_message_size = 1;
    options.io_timeout = 500ms;
    Server server(path, options);
    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.max_message_size = 1;
    client_options.io_timeout = 500ms;
    const Response response = Client(path, client_options).request("x");
    expect(response.status == 404 && response.body.empty(),
           "404 status must remain valid when Not Found text exceeds max_message_size");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_multiplexed_session() {
    using namespace easy_uds;

    const std::string path = socket_path("mux");
    ServerOptions options;
    options.worker_threads = 4;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    options.session_idle_grace = 0ms;  // exercise pure reactor rearm with pipelined read-ahead
    Server server(path, options);
    std::atomic<bool> slow_entered{false};
    server.on("echo", [](const Request& request) { return Response{200, request.body}; });
    server.on("slow", [&](const Request& request) {
        if (request.body == "slow") {
            slow_entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(250ms);
        }
        return Response{200, request.body};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 5s;
    Client client(path, client_options);

    Session session = client.session();
    expect(session.request("echo", "first").body == "first", "session first request");

    // Prove that a later fast request is dispatched while a slow handler on
    // this same connection is still running.
    std::exception_ptr explicit_slow_error;
    std::thread explicit_slow([&] {
        try {
            expect(session.request("slow", "slow").body == "slow", "explicit slow response");
        } catch (...) {
            explicit_slow_error = std::current_exception();
        }
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!slow_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            explicit_slow.join();
            throw std::runtime_error("test failed: slow multiplexed handler did not start");
        }
        std::this_thread::yield();
    }
    const auto fast_started = std::chrono::steady_clock::now();
    expect(session.request("slow", "fast").body == "fast", "fast multiplexed response");
    const auto fast_elapsed = std::chrono::steady_clock::now() - fast_started;
    explicit_slow.join();
    if (explicit_slow_error) {
        std::rethrow_exception(explicit_slow_error);
    }
    expect(fast_elapsed < 200ms, "slow request must not head-of-line block a later fast request");

    // Concurrent multiplexed requests; the slow one must not block the others
    // even though it was issued first.
    std::atomic<std::size_t> completed{0};
    std::vector<std::exception_ptr> errors(8);
    std::vector<std::thread> callers;
    for (std::size_t i = 0; i < errors.size(); ++i) {
        callers.emplace_back([&, i] {
            try {
                const std::string body = i == 0 ? "slow" : "m" + std::to_string(i);
                const Response response = session.request("slow", body);
                expect(response.status == 200 && response.body == body, "multiplexed echo round-trip");
                completed.fetch_add(1, std::memory_order_release);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    expect(completed.load(std::memory_order_acquire) == errors.size(), "all multiplexed callers should succeed");
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    // Raw pipelining: two requests sent back-to-back on one connection are
    // both answered (reactor parses sequentially, multiplex id correlates).
    const int raw_fd = connect_raw(path);
    const std::vector<unsigned char> r1 = fixed_request(7, "echo");
    const std::vector<unsigned char> r2 = fixed_request(8, "echo");
    std::vector<unsigned char> pipeline(r1);
    pipeline.insert(pipeline.end(), r2.begin(), r2.end());
    expect(send_no_signal(raw_fd, pipeline.data(), pipeline.size()) == static_cast<ssize_t>(pipeline.size()),
           "pipelined frames should be written");

    std::array<unsigned char, 20> header{};
    ssize_t got = ::recv(raw_fd, header.data(), header.size(), 0);
    expect(got == static_cast<ssize_t>(header.size()), "first pipelined response header should arrive");
    got = ::recv(raw_fd, header.data(), header.size(), 0);
    expect(got == static_cast<ssize_t>(header.size()), "second pipelined response header should arrive");
    ::close(raw_fd);

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_fragmented_fast_path_header() {
    using namespace easy_uds;

    const std::string path = socket_path("fragmented-fast-path");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 1s;
    options.session_idle_grace = 1ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    const auto first = fixed_request(1, "ping");
    expect(send_no_signal(fd, first.data(), first.size()) == static_cast<ssize_t>(first.size()),
           "first raw request should be written");
    std::array<unsigned char, 20> response_header{};
    recv_exact(fd, response_header.data(), response_header.size());
    std::array<char, 4> response_body{};
    recv_exact(fd, response_body.data(), response_body.size());
    expect(std::string(response_body.data(), response_body.size()) == "pong", "first raw response body");

    const auto second = fixed_request(2, "ping");
    expect(send_no_signal(fd, second.data(), 10) == 10, "partial follow-up header should be written");
    std::this_thread::sleep_for(20ms);  // longer than session_idle_grace
    expect(send_no_signal(fd, second.data() + 10, second.size() - 10) ==
               static_cast<ssize_t>(second.size() - 10),
           "remaining follow-up frame should be written");
    recv_exact(fd, response_header.data(), response_header.size());
    expect(get_u32(response_header.data() + 8) == 2, "fragmented follow-up response id");
    recv_exact(fd, response_body.data(), response_body.size());
    expect(std::string(response_body.data(), response_body.size()) == "pong",
           "fragmented follow-up should complete");
    ::close(fd);

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_connection_reuse() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-reuse");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream("discard", [](const StreamReader& body, const Request&) {
        std::array<char, 64> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        return StreamResponse{204, {}};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    auto stream_start = frame(3, 11, 7, 0, "discard", 7);
    const auto stream_end = frame(5, 11, 0, 0, nullptr, 0);
    stream_start.insert(stream_start.end(), stream_end.begin(), stream_end.end());
    expect(send_no_signal(fd, stream_start.data(), stream_start.size()) ==
               static_cast<ssize_t>(stream_start.size()),
           "stream request should be written");

    std::array<unsigned char, 20> header{};
    recv_exact(fd, header.data(), header.size());
    expect(header[5] == 6 && get_u32(header.data() + 8) == 11, "stream response start");
    recv_exact(fd, header.data(), header.size());
    expect(header[5] == 8 && get_u32(header.data() + 8) == 11, "stream response end");

    const auto fixed = fixed_request(12, "ping");
    expect(send_no_signal(fd, fixed.data(), fixed.size()) == static_cast<ssize_t>(fixed.size()),
           "fixed request after stream should be written");
    recv_exact(fd, header.data(), header.size());
    expect(header[5] == 2 && get_u32(header.data() + 8) == 12, "fixed response after stream");
    std::array<char, 4> fixed_body{};
    recv_exact(fd, fixed_body.data(), fixed_body.size());
    expect(std::string(fixed_body.data(), fixed_body.size()) == "pong", "fixed response body after stream");

    // The fixed-request continuation lease must replay a following stream
    // header to the reactor instead of rejecting it as a fixed frame.
    try {
        const auto second_stream_start = frame(3, 13, 4, 0, "sink", 4);
        const auto second_stream_end = frame(5, 13, 0, 0, nullptr, 0);
        send_exact(fd, second_stream_start.data(), second_stream_start.size());
        send_exact(fd, second_stream_end.data(), second_stream_end.size());
        recv_exact(fd, header.data(), header.size());
        expect(header[5] == 6 && get_u32(header.data() + 8) == 13,
               "stream response after fixed request (type=" + std::to_string(header[5]) +
                   ", id=" + std::to_string(get_u32(header.data() + 8)) + ")");
        recv_exact(fd, header.data(), header.size());
        expect(header[5] == 8 && get_u32(header.data() + 8) == 13, "stream end after fixed request");
    } catch (...) {
        ::close(fd);
        server.stop();
        server_thread.join();
        cleanup_socket_artifacts(path);
        throw;
    }
    ::close(fd);

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_client_stream_response_limit() {
    using namespace easy_uds;

    const std::string path = socket_path("client-stream-limit");
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::system_error(errno, std::generic_category(), "fake stream listener failed");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
        const int error = errno;
        ::close(listener);
        cleanup_socket_artifacts(path);
        throw std::system_error(error, std::generic_category(), "fake stream server setup failed");
    }

    std::thread fake_server([&] {
        const int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        std::array<unsigned char, 41> request{};
        recv_exact(fd, request.data(), request.size());
        const auto start = frame(6, 0, 200, 0, nullptr, 0);
        const auto first = frame(7, 0, 3, 0, "abc", 3);
        const auto second = frame(7, 0, 3, 0, "def", 3);
        const auto end = frame(8, 0, 0, 0, nullptr, 0);
        send_exact(fd, start.data(), start.size());
        send_exact(fd, first.data(), first.size());
        send_exact(fd, second.data(), second.size());
        send_exact(fd, end.data(), end.size());
        ::close(fd);
    });

    ClientOptions options;
    options.max_stream_size = 4;
    options.io_timeout = 1s;
    options.stream_timeout = 1s;
    expect_throws<std::length_error>(
        [&] {
            (void)Client(path, options).request_stream("x", [](char*, std::size_t) { return std::size_t{0}; },
                                                       [](std::string_view) {});
        },
        "client must enforce max_stream_size on the response body");
    fake_server.join();
    ::close(listener);
    cleanup_socket_artifacts(path);
}

void test_run_setup_failure_state() {
    using namespace easy_uds;

    const std::string path = socket_path("run-setup-failure");
    ServerOptions options;
    options.stale_socket_grace_period = 0ms;
    Server server(path, options);

    rlimit original{};
    expect(::getrlimit(RLIMIT_NOFILE, &original) == 0, "getrlimit should succeed");
    rlimit limited = original;
    limited.rlim_cur = 0;
    expect(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "lowering RLIMIT_NOFILE should succeed");
    bool threw = false;
    try {
        server.run();
    } catch (const std::system_error&) {
        threw = true;
    }
    const bool restored = ::setrlimit(RLIMIT_NOFILE, &original) == 0;
    expect(restored, "restoring RLIMIT_NOFILE should succeed");
    expect(threw, "run setup should fail when no descriptor can be allocated");
    expect(!server.is_running(), "failed run setup must clear the running state");
    server.stop();
    cleanup_socket_artifacts(path);
}

void test_session_broken_after_shutdown() {
    using namespace easy_uds;

    const std::string path = socket_path("session-broken");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    Session session = client.session();
    expect(session.request("ping").body == "pong", "session works before shutdown");

    server.stop();
    server_thread.join();

    // The reader thread may observe the close before this call runs, so the
    // first failure is either an I/O system_error or the sticky logic_error;
    // both mark the session unusable.
    expect_throws<std::exception>([&] { (void)session.request("ping"); },
                                  "session request against a stopped server should fail");
    expect_throws<std::logic_error>([&] { (void)session.request("ping"); },
                                    "broken session should reject later requests");
    cleanup_socket_artifacts(path);
}

void test_session_broken_after_timeout() {
    using namespace easy_uds;

    const std::string path = socket_path("session-timeout");
    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.io_timeout = 500ms;
    Server server(path, server_options);
    server.on("slow", [](const Request&) {
        std::this_thread::sleep_for(100ms);
        return Response{200, "late"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 20ms;
    Session session = Client(path, client_options).session();
    expect_throws<std::system_error>([&] { (void)session.request("slow"); },
                                     "session request should report its deadline");
    expect_throws<std::logic_error>([&] { (void)session.request("slow"); },
                                    "timed-out session should remain permanently unusable");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_session_move() {
    using namespace easy_uds;

    const std::string path = socket_path("session-move");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    Session session = client.session();
    expect(session.request("ping").body == "pong", "session works before being moved");

    Session moved = std::move(session);
    expect(moved.request("ping").body == "pong", "moved session keeps the connection");
    expect_throws<std::logic_error>([&] { (void)session.request("ping"); },
                                    "moved-from session must reject requests");

    Session destination = client.session();
    expect(destination.request("ping").body == "pong", "move destination is active before assignment");
    destination = std::move(moved);
    expect(destination.request("ping").body == "pong", "move-assigned session keeps the source connection");
    expect_throws<std::logic_error>([&] { (void)moved.request("ping"); },
                                    "move-assigned source must reject requests");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_reactor_request_timeouts() {
    using namespace easy_uds;

    const std::string path = socket_path("reactor-timeouts");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 200ms;
    options.request_timeout = 500ms;
    options.session_idle_grace = 100ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on("slow-normal", [](const Request&) {
        std::this_thread::sleep_for(300ms);
        return Response{200, "normal-done"};
    });
    server.on_serialized("slow", [](const Request&) {
        std::this_thread::sleep_for(300ms);
        return Response{200, "done"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int idle_fd = connect_raw(path);
    expect_peer_closed(idle_fd, 1s, "idle reactor connection should honor io_timeout");
    ::close(idle_fd);

    const int header_fd = connect_raw(path);
    const unsigned char first_byte = 'E';
    expect(send_no_signal(header_fd, &first_byte, 1) == 1, "partial header byte should be written");
    expect_peer_closed(header_fd, 1s, "partial header should honor io_timeout");
    ::close(header_fd);

    const int payload_fd = connect_raw(path);
    const auto incomplete = frame(1, 41, 8, 0, nullptr, 0);
    expect(send_no_signal(payload_fd, incomplete.data(), incomplete.size()) ==
               static_cast<ssize_t>(incomplete.size()), "incomplete request header should be written");
    for (const char byte : std::string{"ping"}) {
        std::this_thread::sleep_for(100ms);
        expect(send_no_signal(payload_fd, &byte, 1) == 1, "payload progress byte should be written");
    }
    expect_peer_closed(payload_fd, 1s, "partial payload should honor request_timeout");
    ::close(payload_fd);

    // A serialized handler owns a complete request while it runs. Its socket
    // must not be mistaken for an idle, incomplete reactor request.
    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 500ms;
    const Response normal_response = Client(path, client_options).request("slow-normal");
    expect(normal_response.status == 200 && normal_response.body == "normal-done",
           "normal handler should survive a shorter reactor io_timeout");
    Session serialized_session = Client(path, client_options).session();
    expect(serialized_session.request("ping").body == "pong", "session fast path should be established");
    const Response response = serialized_session.request("slow");
    expect(response.status == 200 && response.body == "done",
           "serialized handler should survive a shorter reactor io_timeout");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

unsigned char pattern_byte(std::size_t index) {
    return static_cast<unsigned char>(index * 31U + 7U);
}

void test_streams() {
    using namespace easy_uds;

    constexpr std::size_t upload_size = 12U * 1024U * 1024U + 137U;
    constexpr std::size_t download_size = 10U * 1024U * 1024U + 91U;
    const std::string path = socket_path("stream");

    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.max_connections = 8;
    server_options.stream_chunk_size = 4093;
    server_options.max_stream_size = 32U * 1024U * 1024U;
    server_options.io_timeout = 2s;
    server_options.request_timeout = 5s;
    server_options.stale_socket_grace_period = 0ms;

    Server server(path, server_options);
    server.on_stream("transfer", [](const StreamReader& request_body, const Request&) {
        std::array<char, 3331> buffer{};
        std::size_t received = 0;
        while (true) {
            const std::size_t size = request_body(buffer.data(), buffer.size());
            if (size == 0) {
                break;
            }
            for (std::size_t index = 0; index < size; ++index) {
                if (static_cast<unsigned char>(buffer[index]) != pattern_byte(received + index)) {
                    throw std::runtime_error("upload pattern mismatch");
                }
            }
            received += size;
        }
        if (received != upload_size) {
            throw std::runtime_error("upload length mismatch");
        }

        StreamReader response = [offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
            const std::size_t remaining = download_size - offset;
            const std::size_t size = std::min(capacity, remaining);
            for (std::size_t index = 0; index < size; ++index) {
                output[index] = pattern_byte(offset + index);
            }
            offset += size;
            return size;
        };
        return StreamResponse{206, std::move(response)};
    });
    server.on_stream("prefix-only", [](const StreamReader& request_body, const Request&) {
        std::array<char, 8> prefix{};
        (void)request_body(prefix.data(), prefix.size());
        return StreamResponse{204, {}};
    });
    server.on_stream("throws-stream", [](const StreamReader&, const Request&) -> StreamResponse {
        throw std::runtime_error("stream failure");
    });
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream_prefix("dl.", [](const StreamReader&, const Request& request) {
        const std::string body = "data:" + request.route;
        return StreamResponse{200, [body, offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
                                   const std::size_t take = std::min(capacity, body.size() - offset);
                                   if (take == 0) {
                                       return std::size_t{0};
                                   }
                                   std::memcpy(output, body.data() + offset, take);
                                   offset += take;
                                   return take;
                               }};
    });
    expect_throws<std::runtime_error>(
        [&] { server.on_stream("transfer", [](const StreamReader&, const Request&) { return StreamResponse{}; }); },
        "duplicate streaming route should be rejected");

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.stream_chunk_size = 8191;
    client_options.max_stream_size = 32U * 1024U * 1024U;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 2s;
    client_options.request_timeout = 5s;
    Client client(path, client_options);

    StreamReader upload = [offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
        const std::size_t remaining = upload_size - offset;
        const std::size_t size = std::min(capacity, remaining);
        for (std::size_t index = 0; index < size; ++index) {
            output[index] = pattern_byte(offset + index);
        }
        offset += size;
        return size;
    };

    std::size_t response_offset = 0;
    const Status status = client.request_stream("transfer", upload, [&](std::string_view chunk) {
        for (std::size_t index = 0; index < chunk.size(); ++index) {
            expect(static_cast<unsigned char>(chunk[index]) == pattern_byte(response_offset + index),
                   "download pattern should be preserved");
        }
        response_offset += chunk.size();
    });
    expect(status == 206, "stream status should round-trip");
    expect(response_offset == download_size, "large response should be delivered completely");

    std::size_t remaining = 256U * 1024U;
    StreamReader ignored_tail = [&remaining](char* output, std::size_t capacity) {
        const std::size_t size = std::min(capacity, remaining);
        std::memset(output, 'x', size);
        remaining -= size;
        return size;
    };
    expect(client.request_stream("prefix-only", ignored_tail, {}) == 204,
           "server should drain an unread request tail before responding");

    StreamReader missing_body = [](char*, std::size_t) { return std::size_t{0}; };
    expect(client.request_stream("missing-stream", missing_body, {}) == 404,
           "missing stream route should return 404");
    expect(client.request_stream("throws-stream", missing_body, {}) == 500,
           "throwing stream handler should return 500");

    std::string prefix_body;
    expect(client.request_stream("dl.sensor", missing_body, [&](std::string_view chunk) {
               prefix_body.append(chunk.data(), chunk.size());
           }) == 200,
           "prefix stream route should match");
    expect(prefix_body == "data:dl.sensor", "prefix stream body");

    // Session streams run on their own dedicated connection.
    Session session = client.session();
    std::string session_body;
    expect(session.request_stream("dl.robot", missing_body, [&](std::string_view chunk) {
               session_body.append(chunk.data(), chunk.size());
           }) == 200,
           "session stream should work");
    expect(session_body == "data:dl.robot", "session stream body");
    expect(session.request("ping").body == "pong", "session should stay usable after a stream");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_limit_reserves_worker() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-limit");
    ServerOptions options;
    options.worker_threads = 3;  // automatic max_concurrent_streams = 2
    options.max_connections = 16;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream("block", [](const StreamReader& body, const Request&) {
        std::array<char, 1024> buffer{};
        while (true) {
            const std::size_t size = body(buffer.data(), buffer.size());
            if (size == 0) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        return StreamResponse{200, {}};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    // Two long-lived streams take both stream slots.
    const auto make_slow_reader = [] {
        return StreamReader{[r = std::size_t{4 * 1024 * 1024}](char*, std::size_t c) mutable {
            const std::size_t take = std::min(c, r);
            if (take == 0) return std::size_t{0};
            r -= take;
            return take;
        }};
    };
    StreamReader slow_a = make_slow_reader();
    StreamReader slow_b = make_slow_reader();
    const auto stream_start = std::chrono::steady_clock::now();
    std::thread stream_a([&] { (void)client.request_stream("block", slow_a, {}); });
    std::thread stream_b([&] { (void)client.request_stream("block", slow_b, {}); });
    while (std::chrono::steady_clock::now() - stream_start < 100ms) {
        std::this_thread::yield();
    }

    // The third stream exceeds the slot limit and is rejected by closing.
    StreamReader third = [r = std::size_t{1024 * 1024}](char*, std::size_t c) mutable {
        const std::size_t take = std::min(c, r);
        if (take == 0) return std::size_t{0};
        r -= take;
        return take;
    };
    expect_throws<std::system_error>([&] { (void)client.request_stream("block", third, {}); },
                                     "excess stream should be rejected");

    // The reserved worker still serves regular RPC.
    expect(client.request("ping").body == "pong", "fixed RPC must stay available during stream saturation");

    stream_a.join();
    stream_b.join();

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_timeout_is_independent() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-timeout-independent");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 50ms;
    options.stream_timeout = 500ms;
    Server server(path, options);
    server.on_stream("slow-stream", [](const StreamReader& body, const Request&) {
        std::array<char, 8> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        std::this_thread::sleep_for(120ms);
        return StreamResponse{200, [sent = false](char* output, std::size_t capacity) mutable {
                                  if (sent || capacity == 0) {
                                      return std::size_t{0};
                                  }
                                  output[0] = 'x';
                                  sent = true;
                                  return std::size_t{1};
                              }};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.stream_timeout = 500ms;
    std::string response_body;
    const Status status = Client(path, client_options).request_stream(
        "slow-stream", [](char*, std::size_t) { return std::size_t{0}; },
        [&](std::string_view chunk) { response_body.append(chunk.data(), chunk.size()); });
    expect(status == 200 && response_body == "x",
           "stream_timeout must be independent from the shorter regular request_timeout");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_serialized_handlers() {
    using namespace easy_uds;

    const std::string path = socket_path("serialized");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 16;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    Server server(path, options);
    std::atomic<bool> first_entered{false};
    std::atomic<bool> release_first{false};
    std::atomic<int> serialized_count{0};
    server.on("status", [](const Request&) { return Response{200, "ok"}; });
    server.on_serialized("command", [&](const Request& request) {
        if (request.body == "first") {
            first_entered.store(true, std::memory_order_release);
            while (!release_first.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        serialized_count.fetch_add(1, std::memory_order_relaxed);
        return Response{200, request.body};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    std::thread first([&] {
        try {
            (void)client.request("command", "first");
        } catch (...) {
        }
    });

    const auto first_deadline = std::chrono::steady_clock::now() + 1s;
    while (!first_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= first_deadline) {
            release_first.store(true, std::memory_order_release);
            first.join();
            server.stop();
            server_thread.join();
            cleanup_socket_artifacts(path);
            throw std::runtime_error("test failed: serialized handler did not start");
        }
        std::this_thread::yield();
    }

    // While the first serialized command waits, regular RPC is still served.
    std::thread second([&] {
        try {
            (void)client.request("command", "second");
        } catch (...) {
        }
    });
    std::this_thread::sleep_for(50ms);
    expect(client.request("status").body == "ok", "regular RPC must not wait behind serialized commands");

    release_first.store(true, std::memory_order_release);
    first.join();
    second.join();
    expect(serialized_count.load(std::memory_order_relaxed) == 2, "both serialized commands should execute");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_serialized_queue_expiry() {
    using namespace easy_uds;

    const std::string path = socket_path("serialized-expiry");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 300ms;  // server-side deadline for queued commands
    Server server(path, options);
    std::atomic<bool> release_blocker{false};
    server.on_serialized("command", [&](const Request& request) {
        if (request.body == "blocker") {
            while (!release_blocker.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return Response{200, request.body};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    std::thread blocker([&] {
        try {
            (void)client.request("command", "blocker");
        } catch (...) {
        }
    });
    std::this_thread::sleep_for(100ms);  // let the blocker hold the executor

    // Queued behind the blocker, this request passes its server-side deadline.
    ClientOptions expiring_options;
    expiring_options.io_timeout = 500ms;
    expiring_options.request_timeout = 5s;  // the client waits long enough to see the 408
    Client expiring(path, expiring_options);
    std::atomic<int> expired_status{-1};
    std::thread expired([&] {
        try {
            expired_status.store(expiring.request("command", "expired").status, std::memory_order_relaxed);
        } catch (...) {
            expired_status.store(-1, std::memory_order_relaxed);
        }
    });
    std::this_thread::sleep_for(400ms);  // 300 ms server deadline expires while queued

    release_blocker.store(true, std::memory_order_release);
    blocker.join();
    expired.join();
    expect(expired_status.load(std::memory_order_relaxed) == 408,
           "expired request must be answered with 408, not executed");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_enqueue_maintenance() {
    using namespace easy_uds;

    const std::string path = socket_path("maintenance");
    ServerOptions options;
    options.worker_threads = 2;
    options.max_connections = 16;
    options.io_timeout = 500ms;
    Server server(path, options);

    std::vector<std::string> order;
    std::mutex order_mutex;
    server.on_serialized("record", [&](const Request& request) {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(request.body);
        return Response{200, {}};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    expect(client.request("record", "c1").status == 200, "serialized command c1");
    server.enqueue_maintenance([&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back("m1");
    });
    expect(client.request("record", "c2").status == 200, "serialized command c2");
    {
        std::lock_guard<std::mutex> lock(order_mutex);
        expect(order == std::vector<std::string>({"c1", "m1", "c2"}),
               "maintenance task must keep FIFO order with serialized commands");
    }

    server.enqueue_maintenance([] { throw std::runtime_error("maintenance failure"); });
    std::atomic<bool> ran_after_failure{false};
    server.enqueue_maintenance([&] { ran_after_failure.store(true, std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!ran_after_failure.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            server.stop();
            server_thread.join();
            cleanup_socket_artifacts(path);
            throw std::runtime_error("test failed: maintenance executor stopped after a throwing task");
        }
        std::this_thread::yield();
    }

    expect_throws<std::invalid_argument>([&] { server.enqueue_maintenance({}); },
                                         "empty maintenance task should be rejected");
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
    expect_throws<std::logic_error>([&] { server.enqueue_maintenance([] {}); },
                                    "maintenance on a stopped server should be rejected");
}

void test_client_request_deadline() {
    using namespace easy_uds;

    const std::string path = socket_path("client-deadline");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 2s;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 2s;
    client_options.request_timeout = 500ms;
    Client client(path, client_options);
    expect(client.request("ping").body == "pong", "request within deadline should succeed");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_server_request_timeout_response() {
    using namespace easy_uds;

    const std::string path = socket_path("server-timeout");
    ServerOptions options;
    options.worker_threads = 1;
    options.io_timeout = 2s;
    options.request_timeout = 300ms;  // server-side per-request deadline
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on("slow", [](const Request&) {
        std::this_thread::sleep_for(800ms);
        return Response{200, "slow"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    // The single worker is busy for 800ms.
    std::thread slow([&] {
        try {
            (void)client.request("slow");
        } catch (...) {
        }
    });
    std::this_thread::sleep_for(50ms);

    // Queued behind the slow handler, this request expires server-side and is
    // answered with 408 without invoking the handler.
    ClientOptions quick_options;
    quick_options.io_timeout = 2s;
    quick_options.request_timeout = 3s;
    Client quick(path, quick_options);
    const Response expired = quick.request("ping");
    expect(expired.status == 408, "request past its server-side deadline should receive 408");

    slow.join();
    expect(client.request("ping").body == "pong", "server must stay healthy after expiring a request");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_connection_limit() {
    using namespace easy_uds;

    const std::string path = socket_path("conn-limit");
    ServerOptions options;
    options.worker_threads = 2;
    options.max_connections = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    // Two sessions hold both connection slots open.
    Session first = client.session();
    Session second = client.session();
    expect(first.request("ping").body == "pong", "first session should be served");
    expect(second.request("ping").body == "pong", "second session should be served");

    // A third connection exceeds the limit and is rejected.
    expect_throws<std::system_error>([&] { (void)client.request("ping"); },
                                     "third connection should be rejected at the limit");

    // Sessions are unaffected.
    expect(first.request("ping").body == "pong", "existing sessions keep working");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_concurrent_clients() {
    using namespace easy_uds;

    const std::string path = socket_path("concurrent");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 64;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("echo", [](const Request& request) { return Response{200, request.body}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    constexpr std::size_t client_count = 24;
    std::vector<std::thread> clients;
    std::vector<std::string> replies(client_count);
    std::vector<std::exception_ptr> errors(client_count);
    for (std::size_t index = 0; index < client_count; ++index) {
        clients.emplace_back([&, index] {
            try {
                const std::string body = "client-" + std::to_string(index);
                const Response response = client.request("echo", body);
                expect(response.status == 200 && response.body == body, "concurrent echo");
                replies[index] = response.body;
            } catch (...) {
                errors[index] = std::current_exception();
            }
        });
    }
    for (auto& thread : clients) {
        thread.join();
    }
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    for (std::size_t index = 0; index < client_count; ++index) {
        expect(replies[index] == "client-" + std::to_string(index), "every concurrent client should be answered");
    }

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_disconnected_handler_fd_isolation() {
    using namespace easy_uds;

    const std::string path = socket_path("fd-isolation");
    ServerOptions options;
    options.worker_threads = 2;
    options.max_connections = 8;
    options.io_timeout = 1s;
    options.request_timeout = 2s;
    Server server(path, options);
    std::atomic<bool> old_entered{false};
    std::atomic<bool> release_old{false};
    std::atomic<bool> new_entered{false};
    std::atomic<bool> release_new{false};
    server.on("slow-old", [&](const Request&) {
        old_entered.store(true, std::memory_order_release);
        while (!release_old.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return Response{200, "OLD"};
    });
    server.on("slow-new", [&](const Request&) {
        new_entered.store(true, std::memory_order_release);
        while (!release_new.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return Response{200, "NEW"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int old_fd = connect_raw(path);
    const auto old_request = fixed_request(0, "slow-old");
    send_exact(old_fd, old_request.data(), old_request.size());
    const auto old_deadline = std::chrono::steady_clock::now() + 1s;
    while (!old_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= old_deadline) {
            throw std::runtime_error("test failed: disconnected old handler did not start");
        }
        std::this_thread::yield();
    }
    ::close(old_fd);
    std::this_thread::sleep_for(50ms);  // let the reactor observe old peer HUP

    std::atomic<bool> new_completed{false};
    std::string new_body;
    std::exception_ptr new_error;
    std::thread new_client([&] {
        try {
            new_body = Client(path).request("slow-new").body;
            new_completed.store(true, std::memory_order_release);
        } catch (...) {
            new_error = std::current_exception();
        }
    });
    const auto new_deadline = std::chrono::steady_clock::now() + 1s;
    while (!new_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= new_deadline) {
            release_old.store(true, std::memory_order_release);
            release_new.store(true, std::memory_order_release);
            new_client.join();
            throw std::runtime_error("test failed: replacement handler did not start");
        }
        std::this_thread::yield();
    }

    release_old.store(true, std::memory_order_release);
    std::this_thread::sleep_for(50ms);
    const bool old_response_leaked = new_completed.load(std::memory_order_acquire);
    release_new.store(true, std::memory_order_release);
    new_client.join();
    if (new_error) {
        std::rethrow_exception(new_error);
    }
    expect(!old_response_leaked,
           "old handler response must not be delivered through a reused fd to a new client");
    expect(new_body == "NEW", "new client must receive only its own handler response");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stop_interrupts_blocked_workers() {
    using namespace easy_uds;

    const std::string path = socket_path("stop-blocked");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 5s;
    Server server(path, options);
    std::atomic<bool> entered{false};
    std::atomic<bool> release_handler{false};
    server.on("block", [&](const Request&) {
        entered.store(true, std::memory_order_release);
        while (!release_handler.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return Response{200, "never"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    std::thread blocked([&] {
        try {
            (void)client.request("block");
        } catch (...) {
        }
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            release_handler.store(true, std::memory_order_release);
            server.stop();
            blocked.join();
            server_thread.join();
            cleanup_socket_artifacts(path);
            throw std::runtime_error("test failed: blocked handler did not start");
        }
        std::this_thread::yield();
    }

    // stop() returns immediately even though a handler is still running.
    const auto stop_start = std::chrono::steady_clock::now();
    server.stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_start;
    expect(stop_elapsed < 2s, "stop() must not wait for a still-running handler");

    // Release the handler so the worker and server thread can wind down.
    release_handler.store(true, std::memory_order_release);
    blocked.join();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_handler_error_opt_out() {
    using namespace easy_uds;

    const std::string path = socket_path("err-optout");
    ServerOptions options;
    options.worker_threads = 2;
    options.include_handler_error_messages = false;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("boom", [](const Request&) -> Response { throw std::runtime_error("secret detail"); });
    server.on_stream("boom-stream", [](const StreamReader&, const Request&) -> StreamResponse {
        throw std::runtime_error("stream secret");
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    const Response response = client.request("boom");
    expect(response.status == 500 && response.body == "Internal Server Error",
           "opt-out must hide the handler exception message");

    std::string stream_body;
    const Status status = client.request_stream("boom-stream", [](char*, std::size_t) { return std::size_t{0}; },
                                                [&](std::string_view chunk) {
                                                    stream_body.append(chunk.data(), chunk.size());
                                                });
    expect(status == 500 && stream_body.empty(), "opt-out must hide the stream handler exception message");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

} // namespace

int main() {
#define RUN(name)                      \
    std::cerr << "RUN " #name "\n";    \
    name()
    try {
        RUN(test_option_validation);
        RUN(test_socket_path_safety);
        RUN(test_stop_preserves_replaced_socket_path);
        RUN(test_stale_socket_cleanup);
        RUN(test_basic_server);
        RUN(test_peer_credentials);
        RUN(test_tiny_message_limit_404);
        RUN(test_multiplexed_session);
        RUN(test_fragmented_fast_path_header);
        RUN(test_stream_connection_reuse);
        RUN(test_client_stream_response_limit);
        RUN(test_session_broken_after_shutdown);
        RUN(test_session_broken_after_timeout);
        RUN(test_session_move);
        RUN(test_reactor_request_timeouts);
        RUN(test_streams);
        RUN(test_stream_limit_reserves_worker);
        RUN(test_stream_timeout_is_independent);
        RUN(test_serialized_handlers);
        RUN(test_serialized_queue_expiry);
        RUN(test_enqueue_maintenance);
        RUN(test_client_request_deadline);
        RUN(test_server_request_timeout_response);
        RUN(test_connection_limit);
        RUN(test_concurrent_clients);
        RUN(test_disconnected_handler_fd_isolation);
        RUN(test_stop_interrupts_blocked_workers);
        RUN(test_handler_error_opt_out);
        RUN(test_run_setup_failure_state);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#undef RUN

    std::cout << "All tests passed.\n";
    return 0;
}
