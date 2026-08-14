#include "common.hpp"

namespace easy_uds::test {

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
    options.stats = StatsMode::basic;
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

    const ServerStats limited = server.stats();
    expect(limited.active_connections == 2 && limited.counters &&
               limited.counters->accepted_connections == 2 &&
               limited.counters->rejected_connections >= 1,
           "server stats should expose connection admission and rejection");

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

void test_concurrent_handler_registration() {
    using namespace easy_uds;

    const std::string path = socket_path("handler-cow");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 64;
    options.io_timeout = 2s;
    Server server(path, options);
    server.on("stable", [](const Request& request) { return Response{200, request.body}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    std::atomic<bool> failed{false};
    std::vector<std::thread> callers;
    for (std::size_t caller_index = 0; caller_index < 4; ++caller_index) {
        callers.emplace_back([&, caller_index] {
            try {
                for (std::size_t request_index = 0; request_index < 100; ++request_index) {
                    const std::string body = std::to_string(caller_index) + ":" + std::to_string(request_index);
                    const Response response = client.request("stable", body);
                    if (response.status != 200 || response.body != body) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                }
            } catch (...) {
                failed.store(true, std::memory_order_release);
            }
        });
    }
    for (std::size_t route_index = 0; route_index < 32; ++route_index) {
        server.on("dynamic/" + std::to_string(route_index), [route_index](const Request&) {
            return Response{200, std::to_string(route_index)};
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    const Response dynamic = client.request("dynamic/31");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
    expect(!failed.load(std::memory_order_acquire), "existing handlers must survive concurrent COW registration");
    expect(dynamic.status == 200 && dynamic.body == "31", "new handler snapshot must become visible atomically");
}

} // namespace easy_uds::test
