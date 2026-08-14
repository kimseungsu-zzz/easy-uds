#include "common.hpp"

namespace easy_uds::test {

void test_release_candidate_adversarial() {
    using namespace easy_uds;

    const std::string path = socket_path("rc-adversarial");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_concurrent_serialized_domains = 1;
    options.request_timeout = 2s;
    options.io_timeout = 500ms;
    options.stats = StatsMode::basic;
    Server server(path, options);

    std::atomic<bool> hold_entered{false};
    std::atomic<bool> release_hold{false};
    std::atomic<bool> stop_stats_poll{false};
    std::atomic<bool> stats_poll_failed{false};
    std::atomic<int> velocity_executions{0};
    std::mutex result_mutex;
    std::vector<Status> velocity_statuses;

    server.on(
        "/hold",
        RouteOptions{[&](const Request&) {
            hold_entered.store(true, std::memory_order_release);
            while (!release_hold.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return Response::ok("released");
        }}
            .serialize_in("drivetrain"));
    server.on(
        "/velocity",
        RouteOptions{[&](const Request&) {
            velocity_executions.fetch_add(1, std::memory_order_relaxed);
            return Response::ok("applied");
        }}
            .serialize_in("drivetrain", QueuePolicy::latest_wins));
    server.on("/echo", [](const Request& request) {
        return Response::ok(request.body);
    });
    server.on("/boom", [](const Request&) -> Response {
        throw std::runtime_error("rc handler failure");
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);
    Client client(path);
    Session session = client.session();

    std::thread hold([&] {
        try {
            (void)session.request("/hold");
        } catch (...) {
        }
    });
    std::thread stats_poller([&] {
        while (!stop_stats_poll.load(std::memory_order_acquire)) {
            const ServerStats snapshot = server.stats();
            if (snapshot.active_connections > options.max_connections ||
                snapshot.inflight_requests > options.max_inflight_requests_per_connection *
                                               options.max_connections) {
                stats_poll_failed.store(true, std::memory_order_release);
                return;
            }
        }
    });

    struct Cleanup {
        Server& server;
        std::thread& server_thread;
        std::thread& hold;
        std::thread& stats_poller;
        std::vector<std::thread>& flood;
        std::atomic<bool>& release_hold;
        std::atomic<bool>& stop_stats_poll;
        const std::string& path;

        ~Cleanup() {
            release_hold.store(true, std::memory_order_release);
            if (hold.joinable()) {
                hold.join();
            }
            for (std::thread& request : flood) {
                if (request.joinable()) {
                    request.join();
                }
            }
            stop_stats_poll.store(true, std::memory_order_release);
            if (stats_poller.joinable()) {
                stats_poller.join();
            }
            server.stop();
            if (server_thread.joinable()) {
                server_thread.join();
            }
            cleanup_socket_artifacts(path);
        }
    };

    std::vector<std::thread> flood;
    Cleanup cleanup{server,       server_thread, hold, stats_poller, flood,
                    release_hold, stop_stats_poll, path};

    const auto hold_deadline = std::chrono::steady_clock::now() + 2s;
    while (!hold_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < hold_deadline) {
        std::this_thread::yield();
    }
    expect(hold_entered.load(std::memory_order_acquire),
           "RC hold handler should start before the adversarial flood");
    expect(client.request("/boom").status == status_internal_error,
           "handler exceptions must be converted to a response");

    constexpr int flood_count = 16;
    flood.reserve(flood_count);
    for (int index = 0; index < flood_count; ++index) {
        flood.emplace_back([&, index] {
            try {
                const Response response =
                    session.request("/velocity", std::to_string(index));
                std::lock_guard<std::mutex> lock(result_mutex);
                velocity_statuses.push_back(response.status);
            } catch (...) {
            }
        });
    }

    const auto queue_deadline = std::chrono::steady_clock::now() + 2s;
    while (server.stats().serialized_queue_depth == 0 &&
           std::chrono::steady_clock::now() < queue_deadline) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(50ms);
    expect(server.stats().serialized_queue_depth != 0,
           "LatestWins RC flood should create observable pending work");

    release_hold.store(true, std::memory_order_release);
    hold.join();
    for (std::thread& request : flood) {
        request.join();
    }

    int conflicts = 0;
    int successes = 0;
    {
        std::lock_guard<std::mutex> lock(result_mutex);
        for (const Status status : velocity_statuses) {
            conflicts += status == status_conflict ? 1 : 0;
            successes += status == status_ok ? 1 : 0;
        }
    }
    expect(conflicts > 0, "LatestWins RC flood should supersede pending requests");
    expect(successes > 0 && velocity_executions.load(std::memory_order_relaxed) <
                                  flood_count,
           "LatestWins RC flood should preserve the newest work without running every request");
    expect(session.request("/echo", "session-still-usable").body ==
               "session-still-usable",
           "policy conflicts must not break the persistent Session");
    expect(!stats_poll_failed.load(std::memory_order_acquire),
           "concurrent stats polling must remain bounded and race-free");
    stop_stats_poll.store(true, std::memory_order_release);
    stats_poller.join();
}

}  // namespace easy_uds::test
