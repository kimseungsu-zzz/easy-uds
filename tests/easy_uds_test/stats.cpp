#include "common.hpp"

namespace easy_uds::test {

void test_stats_snapshots() {
    using namespace easy_uds;

    const std::string disabled_path = socket_path("stats-disabled");
    {
        Server disabled(disabled_path);
        const ServerStats snapshot = disabled.stats();
        expect(!snapshot.running, "unstarted server stats should report stopped");
        expect(!snapshot.counters.has_value(),
               "cumulative server counters should default to disabled");
        expect(snapshot.active_connections == 0 && snapshot.active_streams == 0 &&
                   snapshot.inflight_requests == 0 &&
                   snapshot.retained_request_bytes == 0 &&
                   snapshot.queued_output_bytes == 0 &&
                   snapshot.worker_queue_depth == 0 &&
                   snapshot.serialized_queue_depth == 0,
               "unstarted server gauges should be zero");
    }
    cleanup_socket_artifacts(disabled_path);

    const std::string path = socket_path("stats");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 8;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    options.session_idle_grace = 0ms;
    options.stats = StatsMode::basic;

    Server server(path, options);
    std::atomic<bool> handler_entered{false};
    std::atomic<bool> release_handler{false};
    server.on("hold", [&](const Request& request) {
        if (request.body == "first") {
            handler_entered.store(true, std::memory_order_release);
            while (!release_handler.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return Response{200, request.body};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.stats = StatsMode::basic;
    Session session = Client(path, client_options).session();
    std::array<std::string, 2> replies;
    std::array<std::exception_ptr, 2> errors;
    std::thread first([&] {
        try {
            replies[0] = session.request("hold", "first").body;
        } catch (...) {
            errors[0] = std::current_exception();
        }
    });

    const auto entered_deadline = std::chrono::steady_clock::now() + 2s;
    while (!handler_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            release_handler.store(true, std::memory_order_release);
            first.join();
            server.stop();
            server_thread.join();
            cleanup_socket_artifacts(path);
            throw std::runtime_error("test failed: stats blocker did not start");
        }
        std::this_thread::yield();
    }

    std::thread second([&] {
        try {
            replies[1] = session.request("hold", "second").body;
        } catch (...) {
            errors[1] = std::current_exception();
        }
    });

    ServerStats busy;
    const auto queue_deadline = std::chrono::steady_clock::now() + 2s;
    do {
        busy = server.stats();
        if (busy.worker_queue_depth >= 1 && busy.inflight_requests >= 2) {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < queue_deadline);

    expect(busy.running, "running server stats should report running");
    expect(busy.active_connections == 1,
           "persistent Session should occupy one connection");
    expect(busy.worker_queue_depth >= 1,
           "blocked worker should expose queued fixed work");
    expect(busy.inflight_requests >= 2,
           "executing and queued requests should be included in inflight gauge");
    expect(busy.retained_request_bytes >= 2 * std::string{"hold"}.size(),
           "retained request bytes should reuse backpressure accounting");
    expect(busy.counters && busy.counters->accepted_connections == 1 &&
               busy.counters->fixed_requests_dispatched == 2,
           "enabled cumulative counters should observe accepted fixed work");

    const SessionStats session_busy = session.stats();
    expect(session_busy.inflight_requests == 2 && session_busy.counters &&
               session_busy.counters->requests_started == 2 &&
               session_busy.counters->requests_completed == 0,
           "Session snapshot should include both outstanding requests");

    release_handler.store(true, std::memory_order_release);
    first.join();
    second.join();
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
    expect(replies[0] == "first" && replies[1] == "second",
           "stats observation must not disturb request completion");

    const SessionStats session_done = session.stats();
    expect(session_done.inflight_requests == 0 && session_done.counters &&
               session_done.counters->requests_started == 2 &&
               session_done.counters->requests_completed == 2 &&
               session_done.counters->requests_timed_out == 0 &&
               session_done.counters->requests_failed == 0,
           "Session cumulative outcomes should balance after completion");

    const ServerStats done = server.stats();
    expect(done.inflight_requests == 0 && done.retained_request_bytes == 0 &&
               done.worker_queue_depth == 0,
           "server gauges should return to zero after request completion");

    Session moved = std::move(session);
    expect_throws<std::logic_error>([&] { (void)session.stats(); },
                                    "moved-from Session stats should reject access");
    const SessionStats moved_stats = moved.stats();
    expect(moved_stats.counters &&
               moved_stats.counters->requests_completed == 2,
           "Session stats should follow ownership across a move");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);

    const std::string stream_path = socket_path("stats-stream");
    ServerOptions stream_options;
    stream_options.worker_threads = 2;
    stream_options.max_concurrent_streams = 1;
    stream_options.io_timeout = 500ms;
    stream_options.stream_timeout = 5s;
    stream_options.stats = StatsMode::basic;
    Server stream_server(stream_path, stream_options);
    std::atomic<bool> stream_entered{false};
    std::atomic<bool> release_stream{false};
    stream_server.on_stream("hold", [&](const StreamReader&, const Request&) {
        stream_entered.store(true, std::memory_order_release);
        while (!release_stream.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return StreamResponse{200, {}};
    });

    std::thread stream_server_thread([&] { stream_server.run(); });
    wait_until_running(stream_server);
    Client stream_client(stream_path);
    StreamReader empty = [](char*, std::size_t) { return std::size_t{0}; };
    std::exception_ptr stream_error;
    std::thread first_stream([&] {
        try {
            (void)stream_client.request_stream("hold", empty, {});
        } catch (...) {
            stream_error = std::current_exception();
        }
    });

    const auto stream_deadline = std::chrono::steady_clock::now() + 2s;
    while (!stream_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= stream_deadline) {
            release_stream.store(true, std::memory_order_release);
            first_stream.join();
            stream_server.stop();
            stream_server_thread.join();
            cleanup_socket_artifacts(stream_path);
            throw std::runtime_error("test failed: stats stream did not start");
        }
        std::this_thread::yield();
    }
    const ServerStats stream_busy = stream_server.stats();
    expect(stream_busy.active_streams == 1 && stream_busy.counters &&
               stream_busy.counters->stream_requests_started == 1,
           "stream gauges and counters should observe an active exchange");

    StreamReader another_empty = [](char*, std::size_t) { return std::size_t{0}; };
    expect_throws<std::system_error>(
        [&] { (void)stream_client.request_stream("hold", another_empty, {}); },
        "stream beyond the configured slot limit should be rejected");
    const ServerStats stream_rejected = stream_server.stats();
    expect(stream_rejected.counters &&
               stream_rejected.counters->stream_requests_rejected == 1,
           "stream admission rejection should be counted once");

    release_stream.store(true, std::memory_order_release);
    first_stream.join();
    if (stream_error) {
        std::rethrow_exception(stream_error);
    }
    const auto stream_release_deadline = std::chrono::steady_clock::now() + 2s;
    while (stream_server.stats().active_streams != 0 &&
           std::chrono::steady_clock::now() < stream_release_deadline) {
        std::this_thread::yield();
    }
    expect(stream_server.stats().active_streams == 0,
           "active stream gauge should return to zero after completion");

    stream_server.stop();
    stream_server_thread.join();
    cleanup_socket_artifacts(stream_path);
}

} // namespace easy_uds::test
