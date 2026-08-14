#include "common.hpp"

#include <type_traits>

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

void test_serialized_domains_and_policies() {
    using namespace easy_uds;

    static_assert(std::is_constructible_v<
                  RouteOptions, RouteOptions::SimpleHandler>);
    static_assert(std::is_constructible_v<
                  RouteOptions, RouteOptions::ContextHandler>);

    expect_throws<std::invalid_argument>(
        [] {
            (void)RouteOptions{[](const Request&) { return Response{}; }}
                .serialize_in("invalid", static_cast<QueuePolicy>(99));
        },
        "unknown queue policy should be rejected before registration");

    const std::string path = socket_path("serialized-domains");
    ServerOptions options;
    options.worker_threads = 3;
    options.max_concurrent_serialized_domains = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    options.stats = StatsMode::basic;
    Server server(path, options);

    std::atomic<bool> drive_entered{false};
    std::atomic<bool> arm_entered{false};
    std::atomic<bool> camera_entered{false};
    std::atomic<bool> release_drive{false};
    std::atomic<bool> release_arm{false};
    std::atomic<bool> release_camera{false};
    std::mutex executed_mutex;
    std::vector<std::string> executed_velocity;

    server.on(
        "drive.hold",
        RouteOptions{[&](const Request&) {
            drive_entered.store(true, std::memory_order_release);
            while (!release_drive.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Response{status_ok, "drive"};
        }}.serialize_in("drivetrain"));
    server.on(
        "arm.hold",
        RouteOptions{[&](const Request&, const RequestContext& context) {
            if (context.stop_requested()) {
                return Response{status_unavailable, "stopping"};
            }
            arm_entered.store(true, std::memory_order_release);
            while (!release_arm.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Response{status_ok, "arm"};
        }}.serialize_in("arm"));
    server.on(
        "camera.hold",
        RouteOptions{[&](const Request&) {
            camera_entered.store(true, std::memory_order_release);
            while (!release_camera.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Response{status_ok, "camera"};
        }}.serialize_in("camera"));
    server.on_prefix(
        "velocity.",
        RouteOptions{[&](const Request& request) {
            std::lock_guard<std::mutex> lock(executed_mutex);
            executed_velocity.push_back(request.body);
            return Response{status_ok, request.body};
        }}.serialize_in("drivetrain", QueuePolicy::latest_wins));
    server.on(
        "drive.reject",
        RouteOptions{[](const Request&) {
            return Response{status_internal_error,
                            "reject_if_busy handler must not execute"};
        }}.serialize_in("drivetrain", QueuePolicy::reject_if_busy));

    expect_throws<std::invalid_argument>(
        [&] {
            server.on_serialized(
                "ambiguous",
                RouteOptions{[](const Request&) { return Response{}; }}
                    .serialize_in("named"));
        },
        "on_serialized should reject a named advanced domain");

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);
    Client client(path);
    Session session = client.session();

    std::thread drive;
    std::thread arm;
    std::thread camera;
    std::thread old_velocity;
    std::thread latest_velocity;
    std::thread busy_request;
    struct Cleanup {
        Server& server;
        std::thread& server_thread;
        std::thread& drive;
        std::thread& arm;
        std::thread& camera;
        std::thread& old_velocity;
        std::thread& latest_velocity;
        std::thread& busy_request;
        std::atomic<bool>& release_drive;
        std::atomic<bool>& release_arm;
        std::atomic<bool>& release_camera;
        const std::string& path;

        ~Cleanup() {
            release_drive.store(true, std::memory_order_release);
            release_arm.store(true, std::memory_order_release);
            release_camera.store(true, std::memory_order_release);
            for (std::thread* thread :
                 {&drive, &arm, &camera, &old_velocity, &latest_velocity,
                  &busy_request}) {
                if (thread->joinable()) {
                    thread->join();
                }
            }
            server.stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
            cleanup_socket_artifacts(path);
        }
    } cleanup{server, server_thread, drive, arm, camera, old_velocity,
              latest_velocity, busy_request, release_drive, release_arm,
              release_camera, path};

    std::atomic<Status> drive_status{-1};
    std::atomic<Status> arm_status{-1};
    std::atomic<int> domains_ready{0};
    std::atomic<bool> start_domains{false};
    drive = std::thread([&] {
        domains_ready.fetch_add(1, std::memory_order_release);
        while (!start_domains.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        try {
            drive_status.store(session.request("drive.hold").status,
                               std::memory_order_release);
        } catch (...) {
        }
    });
    arm = std::thread([&] {
        domains_ready.fetch_add(1, std::memory_order_release);
        while (!start_domains.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        try {
            arm_status.store(session.request("arm.hold").status,
                             std::memory_order_release);
        } catch (...) {
        }
    });
    while (domains_ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    start_domains.store(true, std::memory_order_release);
    const auto domains_deadline = std::chrono::steady_clock::now() + 2s;
    while ((!drive_entered.load(std::memory_order_acquire) ||
            !arm_entered.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < domains_deadline) {
        std::this_thread::yield();
    }
    if (!drive_entered.load(std::memory_order_acquire) ||
        !arm_entered.load(std::memory_order_acquire)) {
        throw std::runtime_error(
            "test failed: simultaneously admitted domains did not run concurrently");
    }
    expect(server.stats().active_serialized_domains == 2,
           "stats should expose two concurrently active domains");

    std::atomic<Status> camera_status{-1};
    camera = std::thread([&] {
        try {
            camera_status.store(session.request("camera.hold").status,
                                std::memory_order_release);
        } catch (...) {
        }
    });
    const auto camera_queued_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (server.stats().serialized_queue_depth == 0 &&
           std::chrono::steady_clock::now() < camera_queued_deadline) {
        std::this_thread::yield();
    }
    expect(server.stats().serialized_queue_depth != 0,
           "third domain should wait at the configured concurrency cap");
    std::this_thread::sleep_for(20ms);
    expect(!camera_entered.load(std::memory_order_acquire),
           "serialized domain concurrency cap must prevent a third executor");

    release_arm.store(true, std::memory_order_release);
    arm.join();
    expect(arm_status.load(std::memory_order_acquire) == status_ok,
           "arm domain should complete independently");
    const auto camera_deadline = std::chrono::steady_clock::now() + 2s;
    while (!camera_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < camera_deadline) {
        std::this_thread::yield();
    }
    expect(camera_entered.load(std::memory_order_acquire),
           "queued domain should start when capacity becomes available");
    expect(server.stats().active_serialized_domains == 2,
           "active domain count should remain capped during handoff");
    release_camera.store(true, std::memory_order_release);
    camera.join();
    expect(camera_status.load(std::memory_order_acquire) == status_ok,
           "third domain should complete after admission");

    std::atomic<Status> old_status{-1};
    std::atomic<Status> latest_status{-1};
    old_velocity = std::thread([&] {
        try {
            old_status.store(session.request("velocity.left", "old").status,
                             std::memory_order_release);
        } catch (...) {
        }
    });
    const auto queued_deadline = std::chrono::steady_clock::now() + 2s;
    while (server.stats().serialized_queue_depth == 0 &&
           std::chrono::steady_clock::now() < queued_deadline) {
        std::this_thread::yield();
    }
    if (server.stats().serialized_queue_depth == 0) {
        throw std::runtime_error(
            "test failed: old velocity command did not wait behind its domain");
    }

    std::atomic<Status> busy_status{-1};
    busy_request = std::thread([&] {
        try {
            busy_status.store(session.request("drive.reject").status,
                              std::memory_order_release);
        } catch (...) {
        }
    });
    const auto busy_deadline = std::chrono::steady_clock::now() + 2s;
    while (busy_status.load(std::memory_order_acquire) == -1 &&
           std::chrono::steady_clock::now() < busy_deadline) {
        std::this_thread::yield();
    }
    if (busy_status.load(std::memory_order_acquire) != status_conflict) {
        throw std::runtime_error(
            "test failed: RejectIfBusy did not immediately return 409");
    }
    busy_request.join();

    latest_velocity = std::thread([&] {
        try {
            latest_status.store(
                session.request("velocity.left", "latest").status,
                std::memory_order_release);
        } catch (...) {
        }
    });
    const auto supersede_deadline = std::chrono::steady_clock::now() + 2s;
    while (old_status.load(std::memory_order_acquire) == -1 &&
           std::chrono::steady_clock::now() < supersede_deadline) {
        std::this_thread::yield();
    }
    if (old_status.load(std::memory_order_acquire) != status_conflict) {
        throw std::runtime_error(
            "test failed: LatestWins did not reject the superseded request");
    }

    const ServerStats policy_stats = server.stats();
    expect(policy_stats.counters &&
               policy_stats.counters->serialized_requests_superseded == 1 &&
               policy_stats.counters->serialized_requests_rejected_busy == 1,
           "serialized policy outcomes should have distinct counters");

    release_drive.store(true, std::memory_order_release);
    drive.join();
    latest_velocity.join();
    old_velocity.join();
    expect(drive_status.load(std::memory_order_acquire) == status_ok &&
               latest_status.load(std::memory_order_acquire) == status_ok,
           "running and latest commands should complete successfully");
    {
        std::lock_guard<std::mutex> lock(executed_mutex);
        expect(executed_velocity == std::vector<std::string>({"latest"}),
               "LatestWins must execute only the newest queued command");
    }

    const auto idle_deadline = std::chrono::steady_clock::now() + 2s;
    while (server.stats().active_serialized_domains != 0 &&
           std::chrono::steady_clock::now() < idle_deadline) {
        std::this_thread::yield();
    }
    expect(server.stats().active_serialized_domains == 0,
           "serialized domain gauge should return to zero");

}

void test_serialized_queue_expiry() {
    using namespace easy_uds;

    const std::string path = socket_path("serialized-expiry");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 300ms;  // server-side deadline for queued commands
    options.stats = StatsMode::basic;
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

    const ServerStats queued_stats = server.stats();
    expect(queued_stats.serialized_queue_depth >= 1,
           "serialized queue gauge should include an expired item until dequeued");

    release_blocker.store(true, std::memory_order_release);
    blocker.join();
    expired.join();
    expect(expired_status.load(std::memory_order_relaxed) == 408,
           "expired request must be answered with 408, not executed");
    const ServerStats stats = server.stats();
    expect(stats.counters &&
               stats.counters->fixed_requests_dispatched == 2 &&
               stats.counters->requests_timed_out_before_execution == 1,
           "server stats should count one pre-execution queue timeout");

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
