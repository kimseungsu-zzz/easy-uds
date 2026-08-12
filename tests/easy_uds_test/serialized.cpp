#include "common.hpp"

namespace easy_uds::test {

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

} // namespace easy_uds::test
