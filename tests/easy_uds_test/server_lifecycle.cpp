#include "common.hpp"

namespace easy_uds::test {

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

    server_options = {};
    server_options.stats = static_cast<StatsMode>(99);
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-stats"), server_options); },
        "unknown stats mode should be rejected");

    ClientOptions client_options;
    client_options.connect_timeout = -1ms;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-client"), client_options); },
                                         "negative connect_timeout should be rejected");

    client_options = {};
    client_options.stream_chunk_size = 0;
    expect_throws<std::invalid_argument>([&] { Client client(socket_path("bad-client2"), client_options); },
                                         "zero stream_chunk_size should be rejected");

    client_options = {};
    client_options.stats = static_cast<StatsMode>(99);
    expect_throws<std::invalid_argument>(
        [&] { Client client(socket_path("bad-client-stats"), client_options); },
        "unknown client stats mode should be rejected");
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

    try {
        Server duplicate(path);
        throw std::runtime_error(
            "test failed: active socket path should not be replaced");
    } catch (const Error& error) {
        expect(error.kind() == ErrorCode::busy,
               "active socket path should report busy");
        expect(error.system_code() ==
                   std::error_code(EADDRINUSE, std::generic_category()),
               "active socket path should preserve EADDRINUSE");
    }

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

} // namespace easy_uds::test
