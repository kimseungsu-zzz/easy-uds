#include "easy_uds/easy_uds.hpp"

#include <array>
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

    server_options = {};
    server_options.request_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Server server(socket_path("bad-request-timeout"), server_options); },
                                         "negative server request timeout should be rejected");

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
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }

    std::cout << "All tests passed.\n";
    return 0;
}
