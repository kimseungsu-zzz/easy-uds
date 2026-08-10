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
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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

void test_option_validation() {
    using namespace easy_uds;

    ServerOptions server_options;
    server_options.worker_threads = 0;
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-workers"), server_options); }, "zero worker_threads should be rejected");

    server_options = {};
    server_options.max_connections = 2;
    server_options.worker_threads = 3;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-capacity"), server_options); },
                                         "worker_threads > max_connections should be rejected");

    server_options = {};
    server_options.socket_permissions = 01000;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-mode"), server_options); },
                                         "invalid socket permissions should be rejected");

    const std::string stream_limit_path = socket_path("bad-stream-capacity");
    server_options = {};
    server_options.worker_threads = 2;
    {
        Server server(stream_limit_path, server_options);
        expect_throws<std::invalid_argument>([&] { server.set_max_concurrent_streams(0); },
                                             "zero stream concurrency should be rejected");
        server.set_max_concurrent_streams(1);
        server.set_max_concurrent_streams(server_options.worker_threads);
        expect_throws<std::invalid_argument>([&] { server.set_max_concurrent_streams(3); },
                                             "stream concurrency above worker count should be rejected");
        server.stop();
        expect_throws<std::logic_error>([&] { server.set_max_concurrent_streams(1); },
                                        "stream concurrency cannot change after stop");
    }
    cleanup_socket_artifacts(stream_limit_path);

    server_options = {};
    server_options.request_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-request-timeout"), server_options); },
                                         "negative server request timeout should be rejected");

    server_options = {};
    server_options.stream_chunk_size = 0;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-stream-chunk"), server_options); },
                                         "zero stream chunk size should be rejected");

    server_options = {};
    server_options.stream_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-stream-timeout"), server_options); },
                                         "negative stream timeout should be rejected");

    server_options = {};
    server_options.stale_socket_grace_period = -1ms;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-stale-grace"), server_options); },
                                         "negative stale socket grace period should be rejected");

    ClientOptions client_options;
    client_options.connect_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-timeout"), client_options); },
                                         "negative client timeout should be rejected");

    client_options = {};
    client_options.request_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-client-request-timeout"), client_options); },
                                         "negative client request timeout should be rejected");

    client_options = {};
    client_options.stream_chunk_size = 0;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-client-stream"), client_options); },
                                         "zero client stream chunk size should be rejected");

    expect_throws<std::invalid_argument>([] { Client client(std::string(512, 'x')); },
                                         "overlong socket path should be rejected");
    expect_throws<std::invalid_argument>([] { Client client(std::string("/tmp/easy\0uds.sock", 18)); },
                                         "embedded NUL in pathname socket should be rejected");
}

void test_socket_path_safety() {
    using namespace easy_uds;

    const std::string regular_path = socket_path("regular-file");
    {
        std::ofstream file(regular_path);
        file << "do not delete";
    }
    expect_throws<std::runtime_error>([&] { Server server(regular_path); },
                                      "a regular file at the socket path must not be removed");
    struct stat regular_info {};
    expect(::lstat(regular_path.c_str(), &regular_info) == 0 && S_ISREG(regular_info.st_mode),
           "regular file should still exist");
    (void)::unlink(regular_path.c_str());

    const std::string stale_path = socket_path("stale");
    make_stale_socket(stale_path);
    {
        Server server(stale_path);
        struct stat info {};
        expect(::lstat(stale_path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode),
               "stale socket should be replaced by a live socket");
    }
    expect(::lstat(stale_path.c_str(), &regular_info) != 0 && errno == ENOENT,
           "owned socket path should be removed on destruction");

    const std::string owned_path = socket_path("ownership");
    const std::string moved_path = owned_path + ".moved";
    {
        Server server(owned_path);
        expect(::rename(owned_path.c_str(), moved_path.c_str()) == 0, "test should move the owned socket path");
        {
            std::ofstream replacement(owned_path);
            replacement << "replacement";
        }
        server.stop();
        struct stat replacement_info {};
        expect(::lstat(owned_path.c_str(), &replacement_info) == 0 && S_ISREG(replacement_info.st_mode),
               "stop() must not unlink a replacement path with a different inode");
    }
    (void)::unlink(owned_path.c_str());
    (void)::unlink(moved_path.c_str());
}

void test_stale_grace_preserves_server_that_is_starting() {
    using namespace easy_uds;

    const std::string path = socket_path("starting-server");
    cleanup_socket_artifacts(path);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "starting-server raw socket failed");
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "starting-server bind failed");
    }

    std::thread delayed_listen([fd] {
        std::this_thread::sleep_for(80ms);
        (void)::listen(fd, 4);
    });

    ServerOptions options;
    options.stale_socket_grace_period = 250ms;
    expect_throws<std::runtime_error>([&] { Server competing(path, options); },
                                      "grace period should observe a server that begins listening");

    delayed_listen.join();
    struct stat info {};
    expect(::lstat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode),
           "starting server socket must not be removed as stale");

    ::close(fd);
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
    options.socket_permissions = 0600;

    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on("echo", [](const Request& request) { return Response{200, request.body}; });
    server.on(std::string("route\nwith\0binary", 17), [](const Request& request) { return Response{201, request.body}; });
    server.on("throws", [](const Request&) -> Response { throw std::runtime_error("boom"); });
    server.on("too-large", [](const Request&) { return Response{200, std::string(300, 'x')}; });
    server.on("bad-status", [](const Request&) { return Response{-1, "bad"}; });

    expect(server.socket_path() == path, "Server::socket_path should return the configured path");
    expect_throws<std::runtime_error>([&] { server.on("ping", [](const Request&) { return Response{}; }); },
                                      "duplicate route should be rejected");
    expect_throws<std::invalid_argument>([&] { server.on("", [](const Request&) { return Response{}; }); },
                                         "empty server route should be rejected");
    expect_throws<std::length_error>(
        [&] { server.on(std::string(257, 'r'), [](const Request&) { return Response{}; }); },
        "route larger than server max_message_size should be rejected");

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

    ClientOptions client_options;
    client_options.max_message_size = 256;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 500ms;
    Client client(path, client_options);

    Response response = client.request("ping");
    expect(response.status_code == 200 && response.body == "pong", "ping response");

    const std::string binary_body("hello\0world\nagain", 17);
    response = client.request("echo", binary_body);
    expect(response.status_code == 200 && response.body == binary_body, "binary body should round-trip exactly");

    const std::string binary_route("route\nwith\0binary", 17);
    response = client.request(binary_route, "ok");
    expect(response.status_code == 201 && response.body == "ok", "binary route should be supported by the wire protocol");

    response = client.request("missing");
    expect(response.status_code == 404 && response.body == "Not Found", "missing route response");

    response = client.request("throws");
    expect(response.status_code == 500 && response.body == "Internal Server Error", "throwing handler response");

    response = client.request("too-large");
    expect(response.status_code == 500 && response.body == "Internal Server Error", "oversized handler response");

    response = client.request("bad-status");
    expect(response.status_code == 500 && response.body == "Internal Server Error", "invalid handler status response");

    expect_throws<std::invalid_argument>([&] { (void)client.request(""); }, "empty client route should be rejected");
    expect_throws<std::length_error>([&] { (void)client.request("echo", std::string(253, 'x')); },
                                     "oversized request should be rejected before sending");

    std::array<unsigned char, 16> garbage{};
    const int malformed_fd = connect_raw(path);
    expect(::send(malformed_fd, garbage.data(), garbage.size(), 0) == static_cast<ssize_t>(garbage.size()),
           "malformed protocol header should be written");
    ::close(malformed_fd);
    std::this_thread::sleep_for(10ms);
    expect(client.request("ping").body == "pong", "malformed client must not kill the server");

    std::vector<std::thread> clients;
    std::vector<std::string> replies(24);
    std::vector<std::exception_ptr> client_errors(replies.size());
    for (std::size_t index = 0; index < replies.size(); ++index) {
        clients.emplace_back([&, index] {
            try {
                replies[index] = client.request("echo", std::to_string(index)).body;
            } catch (...) {
                client_errors[index] = std::current_exception();
            }
        });
    }
    for (auto& thread : clients) {
        thread.join();
    }
    for (std::size_t index = 0; index < replies.size(); ++index) {
        if (client_errors[index]) {
            std::rethrow_exception(client_errors[index]);
        }
        expect(replies[index] == std::to_string(index), "concurrent echo response " + std::to_string(index));
    }

    server.stop();
    server_thread.join();
    if (run_error) {
        std::rethrow_exception(run_error);
    }
    expect(!server.is_running(), "server should report stopped state");
    expect(::lstat(path.c_str(), &info) != 0 && errno == ENOENT, "socket path should be unlinked by stop()");

    expect_throws<std::logic_error>([&] { server.run(); }, "server should not be restartable");
    expect_throws<std::system_error>([&] { (void)client.request("ping"); }, "request after stop should fail");
}

void test_idle_client_timeout() {
    using namespace easy_uds;

    const std::string path = socket_path("timeout");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 2;
    options.io_timeout = 100ms;

    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int idle_fd = connect_raw(path);
    std::this_thread::sleep_for(10ms);

    ClientOptions client_options;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 500ms;
    Client client(path, client_options);

    const auto start = std::chrono::steady_clock::now();
    const Response response = client.request("ping");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(response.body == "pong", "server should recover after an idle client times out");
    expect(elapsed < 500ms, "idle client must not block the only worker indefinitely");

    ::close(idle_fd);
    server.stop();
    server_thread.join();
}

void test_connection_limit() {
    using namespace easy_uds;

    const std::string path = socket_path("connection-limit");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 1;
    options.io_timeout = 0ms;

    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int idle_fd = connect_raw(path);
    std::this_thread::sleep_for(10ms);

    ClientOptions client_options;
    client_options.connect_timeout = 300ms;
    client_options.io_timeout = 300ms;
    Client client(path, client_options);
    expect_throws<std::system_error>([&] { (void)client.request("ping"); },
                                     "connection over max_connections should be closed");

    ::close(idle_fd);
    server.stop();
    server_thread.join();
}

void test_stop_interrupts_blocked_worker() {
    using namespace easy_uds;

    const std::string path = socket_path("stop-idle");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 1;
    options.io_timeout = 0ms;

    Server server(path, options);
    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int idle_fd = connect_raw(path);
    std::this_thread::sleep_for(20ms);

    const auto start = std::chrono::steady_clock::now();
    server.stop();
    server_thread.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    expect(elapsed < 500ms, "stop() should interrupt a worker blocked in recv()");
    ::close(idle_fd);
}

void test_instance_lock_and_reuse_after_stop() {
    using namespace easy_uds;

    const std::string path = socket_path("instance-lock");
    ServerOptions options;
    options.stale_socket_grace_period = 0ms;

    Server first(path, options);
    try {
        Server second(path, options);
        (void)second;
        throw std::runtime_error("test failed: second server should not acquire the same instance lock");
    } catch (const std::system_error& error) {
        expect(error.code().value() == EADDRINUSE, "instance lock should report EADDRINUSE");
    }

    first.stop();

    {
        Server replacement(path, options);
        replacement.stop();
    }

    cleanup_socket_artifacts(path);
}

void test_server_absolute_request_deadline() {
    using namespace easy_uds;

    const std::string path = socket_path("absolute-deadline");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 1;
    options.io_timeout = 100ms;
    options.request_timeout = 180ms;
    options.stale_socket_grace_period = 0ms;

    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int slow_fd = connect_raw(path);
    const std::array<unsigned char, 16> valid_header{{'E', 'U', 'D', 'S', 1, 1, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0}};
    std::thread slow_writer([&] {
        for (const unsigned char byte : valid_header) {
            if (send_no_signal(slow_fd, &byte, 1) != 1) {
                break;
            }
            std::this_thread::sleep_for(50ms);
        }
    });

    std::this_thread::sleep_for(320ms);

    ClientOptions client_options;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 1000ms;
    Client client(path, client_options);
    const Response response = client.request("ping");
    expect(response.status_code == 200 && response.body == "pong",
           "absolute request deadline should release a worker despite slow-drip progress");

    slow_writer.join();
    ::close(slow_fd);
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_client_absolute_request_deadline() {
    using namespace easy_uds;

    const std::string path = socket_path("client-deadline");
    ServerOptions server_options;
    server_options.io_timeout = 0ms;
    server_options.request_timeout = 0ms;
    server_options.stale_socket_grace_period = 0ms;

    Server server(path, server_options);
    server.on("slow", [](const Request&) {
        std::this_thread::sleep_for(300ms);
        return Response{200, "late"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 0ms;
    client_options.request_timeout = 100ms;
    Client client(path, client_options);

    const auto start = std::chrono::steady_clock::now();
    try {
        (void)client.request("slow");
        throw std::runtime_error("test failed: client absolute request deadline should time out");
    } catch (const std::system_error& error) {
        expect(error.code().value() == ETIMEDOUT, "client request deadline should report ETIMEDOUT");
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(elapsed < 250ms, "client request deadline should cap the whole transaction");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

char pattern_byte(std::size_t offset) {
    return static_cast<char>((offset * 31U + 17U) % 251U);
}

void test_chunked_large_streams() {
    using namespace easy_uds;

    constexpr std::size_t upload_size = 12U * 1024U * 1024U + 137U;
    constexpr std::size_t download_size = 10U * 1024U * 1024U + 91U;
    const std::string path = socket_path("large-stream");

    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.max_connections = 8;
    server_options.stream_chunk_size = 4093;
    server_options.max_stream_size = 32U * 1024U * 1024U;
    server_options.io_timeout = 2s;
    server_options.stale_socket_grace_period = 0ms;

    Server server(path, server_options);
    server.set_max_concurrent_streams(2);
    server.on_stream("transfer", [](const StreamReader& request_body) {
        std::array<char, 3331> buffer{};
        std::size_t received = 0;
        while (true) {
            const std::size_t size = request_body(buffer.data(), buffer.size());
            if (size == 0) {
                break;
            }
            for (std::size_t index = 0; index < size; ++index) {
                if (buffer[index] != pattern_byte(received + index)) {
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
    server.on_stream("prefix-only", [](const StreamReader& request_body) {
        std::array<char, 8> prefix{};
        (void)request_body(prefix.data(), prefix.size());
        return StreamResponse{204, {}};
    });
    server.on_stream("throws-stream", [](const StreamReader&) -> StreamResponse {
        throw std::runtime_error("stream failure");
    });
    server.on_stream("bad-stream-status", [](const StreamReader&) { return StreamResponse{-1, {}}; });
    expect_throws<std::runtime_error>(
        [&] { server.on_stream("transfer", [](const StreamReader&) { return StreamResponse{}; }); },
        "duplicate streaming route should be rejected");
    expect_throws<std::invalid_argument>(
        [&] { server.on_stream("", [](const StreamReader&) { return StreamResponse{}; }); },
        "empty streaming route should be rejected");

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.stream_chunk_size = 8191;
    client_options.max_stream_size = 32U * 1024U * 1024U;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 2s;
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
    const int status = client.request_stream("transfer", upload, [&](std::string_view chunk) {
        for (std::size_t index = 0; index < chunk.size(); ++index) {
            expect(chunk[index] == pattern_byte(response_offset + index), "download pattern should be preserved");
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
    expect(client.request_stream("bad-stream-status", missing_body, {}) == 500,
           "negative stream status should return 500");

    ClientOptions limited_options = client_options;
    limited_options.stream_chunk_size = 8;
    limited_options.max_stream_size = 4;
    Client limited_client(path, limited_options);
    bool produced = false;
    StreamReader oversized = [&produced](char* output, std::size_t capacity) {
        if (produced) {
            return std::size_t{0};
        }
        produced = true;
        std::memset(output, 'z', capacity);
        return capacity;
    };
    expect_throws<std::length_error>([&] { (void)limited_client.request_stream("prefix-only", oversized, {}); },
                                     "client should reject a stream over max_stream_size");
    expect(client.request_stream("missing-stream", missing_body, {}) == 404,
           "an oversized producer must not damage the server");

    expect_throws<std::invalid_argument>([&] { (void)client.request_stream("", missing_body, {}); },
                                         "empty streaming route should be rejected");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_limit_preserves_rpc_worker() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-worker-reservation");
    ServerOptions server_options;
    server_options.worker_threads = 4;
    server_options.max_connections = 32;
    server_options.io_timeout = 10s;
    server_options.request_timeout = 15s;
    server_options.stale_socket_grace_period = 0ms;

    std::atomic<std::size_t> streams_entered{0};
    Server server(path, server_options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream("hold", [&streams_entered](const StreamReader& body) {
        streams_entered.fetch_add(1, std::memory_order_release);
        std::array<char, 64> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        return StreamResponse{204, {}};
    });

    std::exception_ptr server_error;
    std::thread server_thread([&] {
        try {
            server.run();
        } catch (...) {
            server_error = std::current_exception();
        }
    });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 10s;
    client_options.request_timeout = 15s;
    Client client(path, client_options);

    constexpr std::size_t held_stream_count = 3;
    std::atomic<bool> finish_streams{false};
    std::vector<std::exception_ptr> stream_errors(held_stream_count);
    std::vector<std::thread> stream_threads;
    stream_threads.reserve(held_stream_count);
    for (std::size_t stream_index = 0; stream_index < held_stream_count; ++stream_index) {
        stream_threads.emplace_back([&, stream_index] {
            try {
                bool sent_first_chunk = false;
                StreamReader source = [&](char* output, std::size_t) {
                    if (!sent_first_chunk) {
                        sent_first_chunk = true;
                        output[0] = 'x';
                        return std::size_t{1};
                    }
                    while (!finish_streams.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                    return std::size_t{0};
                };
                expect(client.request_stream("hold", source, {}) == 204,
                       "reserved stream should complete normally");
            } catch (...) {
                stream_errors[stream_index] = std::current_exception();
            }
        });
    }

    const auto entered_deadline = std::chrono::steady_clock::now() + 2s;
    while (streams_entered.load(std::memory_order_acquire) != held_stream_count) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            finish_streams.store(true, std::memory_order_release);
            for (auto& thread : stream_threads) {
                thread.join();
            }
            server.stop();
            server_thread.join();
            cleanup_socket_artifacts(path);
            throw std::runtime_error("test failed: three streams did not occupy their workers");
        }
        std::this_thread::yield();
    }

    bool excess_stream_rejected = false;
    std::exception_ptr excess_stream_error;
    try {
        StreamReader empty_stream;
        (void)client.request_stream("hold", empty_stream, {});
    } catch (const std::system_error&) {
        excess_stream_rejected = true;
    } catch (...) {
        excess_stream_error = std::current_exception();
    }

    constexpr std::size_t rpc_thread_count = 4;
    constexpr std::size_t requests_per_thread = 500;
    std::atomic<std::size_t> rpc_successes{0};
    std::vector<std::exception_ptr> rpc_errors(rpc_thread_count);
    std::vector<std::thread> rpc_threads;
    rpc_threads.reserve(rpc_thread_count);
    for (std::size_t rpc_index = 0; rpc_index < rpc_thread_count; ++rpc_index) {
        rpc_threads.emplace_back([&, rpc_index] {
            try {
                for (std::size_t request_index = 0; request_index < requests_per_thread; ++request_index) {
                    const Response response = client.request("ping");
                    if (response.status_code != 200 || response.body != "pong") {
                        throw std::runtime_error("regular RPC returned an unexpected response");
                    }
                    rpc_successes.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                rpc_errors[rpc_index] = std::current_exception();
            }
        });
    }
    for (auto& thread : rpc_threads) {
        thread.join();
    }

    finish_streams.store(true, std::memory_order_release);
    for (auto& thread : stream_threads) {
        thread.join();
    }

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);

    for (const auto& error : stream_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    if (excess_stream_error) {
        std::rethrow_exception(excess_stream_error);
    }
    for (const auto& error : rpc_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    expect(excess_stream_rejected, "stream above max_concurrent_streams should be rejected");
    expect(rpc_successes.load(std::memory_order_relaxed) == rpc_thread_count * requests_per_thread,
           "regular RPCs should keep making progress while three streams occupy workers");
}

} // namespace

int main() {
    try {
        test_option_validation();
        test_socket_path_safety();
        test_stale_grace_preserves_server_that_is_starting();
        test_basic_server();
        test_idle_client_timeout();
        test_connection_limit();
        test_stop_interrupts_blocked_worker();
        test_instance_lock_and_reuse_after_stop();
        test_server_absolute_request_deadline();
        test_client_absolute_request_deadline();
        test_chunked_large_streams();
        test_stream_limit_preserves_rpc_worker();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
