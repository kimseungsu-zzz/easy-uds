#include "common.hpp"

namespace easy_uds::test {

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

void test_closing_connection_counts_toward_limit() {
    using namespace easy_uds;

    const std::string path = socket_path("closing-limit");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 1;
    options.io_timeout = 1s;
    options.request_timeout = 2s;
    Server server(path, options);
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    server.on("slow", [&](const Request&) {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return Response{200, "old"};
    });
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int old_fd = connect_raw(path);
    const auto slow = fixed_request(0, "slow");
    send_exact(old_fd, slow.data(), slow.size());
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            throw std::runtime_error("test failed: closing-limit handler did not start");
        }
        std::this_thread::yield();
    }
    ::close(old_fd);
    std::this_thread::sleep_for(50ms);

    std::exception_ptr failure;
    try {
        ClientOptions client_options;
        client_options.io_timeout = 300ms;
        client_options.request_timeout = 300ms;
        expect_throws<std::system_error>(
            [&] { (void)Client(path, client_options).request("ping"); },
            "closing active connection must still consume its connection slot");

        release.store(true, std::memory_order_release);
        ClientOptions retry_options;
        retry_options.io_timeout = 300ms;
        retry_options.request_timeout = 300ms;
        bool reopened = false;
        const auto retry_deadline = std::chrono::steady_clock::now() + 1s;
        while (!reopened && std::chrono::steady_clock::now() < retry_deadline) {
            try {
                reopened = Client(path, retry_options).request("ping").body == "pong";
            } catch (const std::system_error&) {
                std::this_thread::sleep_for(10ms);
            }
        }
        expect(reopened, "connection slot should reopen after the old handler releases its fd reference");
    } catch (...) {
        failure = std::current_exception();
    }

    release.store(true, std::memory_order_release);
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
    if (failure) {
        std::rethrow_exception(failure);
    }
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

} // namespace easy_uds::test
