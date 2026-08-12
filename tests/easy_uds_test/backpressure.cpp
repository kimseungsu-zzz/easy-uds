#include "common.hpp"

namespace easy_uds::test {

void test_stalled_response_does_not_block_workers() {
    using namespace easy_uds;

    const std::string path = socket_path("stalled-response");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 16;
    options.io_timeout = 1s;
    options.request_timeout = 3s;
    Server server(path, options);
    std::atomic<std::size_t> large_handlers{0};
    const std::string large_body(900U * 1024U, 'x');
    server.on("large", [&](const Request&) {
        large_handlers.fetch_add(1, std::memory_order_release);
        return Response{200, large_body};
    });
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    std::vector<int> stalled;
    for (std::uint32_t index = 0; index < options.worker_threads; ++index) {
        const int fd = connect_raw(path);
        const auto request = fixed_request(index + 1, "large");
        send_exact(fd, request.data(), request.size());
        stalled.push_back(fd);
    }
    const auto handlers_deadline = std::chrono::steady_clock::now() + 1s;
    while (large_handlers.load(std::memory_order_acquire) != options.worker_threads) {
        if (std::chrono::steady_clock::now() >= handlers_deadline) {
            throw std::runtime_error("test failed: large response handlers did not all start");
        }
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(20ms);

    ClientOptions client_options;
    client_options.io_timeout = 1s;
    client_options.request_timeout = 2s;
    const auto started = std::chrono::steady_clock::now();
    const Response response = Client(path, client_options).request("ping");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    expect(response.status == 200 && response.body == "pong", "healthy request behind stalled readers");
    expect(elapsed < 500ms, "non-reading peers must not occupy the fixed worker pool");

    for (const int fd : stalled) {
        ::close(fd);
    }
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_fixed_output_queue_is_bounded() {
    using namespace easy_uds;

    const std::string path = socket_path("bounded-output");
    ServerOptions options;
    options.worker_threads = 4;
    options.max_connections = 16;
    options.io_timeout = 5s;
    options.request_timeout = 5s;
    Server server(path, options);
    const std::string large_body(900U * 1024U, 'x');
    server.on("large", [&](const Request&) { return Response{200, large_body}; });
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    for (std::uint32_t index = 0; index < 16; ++index) {
        const auto request = fixed_request(index + 1, "large");
        send_exact(fd, request.data(), request.size());
    }
    std::this_thread::sleep_for(200ms);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    expect(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0,
           "make bounded-output peer nonblocking");
    bool closed = false;
    std::array<char, 64U * 1024U> drain{};
    const auto close_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < close_deadline) {
        const ssize_t received = ::recv(fd, drain.data(), drain.size(), 0);
        if (received > 0) {
            continue;
        }
        if (received == 0) {
            closed = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
            (void)::poll(&descriptor, 1, 20);
            continue;
        }
        break;
    }
    ::close(fd);

    const Response healthy = Client(path).request("ping");
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
    expect(closed, "fixed response backlog beyond the per-connection byte cap must close the peer");
    expect(healthy.status == 200 && healthy.body == "pong", "output queue overflow must stay isolated");
}

void test_pipelined_input_applies_backpressure() {
    using namespace easy_uds;

    const std::string path = socket_path("pipeline-backpressure");
    ServerOptions options;
    options.worker_threads = 1;
    options.max_connections = 8;
    options.io_timeout = 2s;
    options.request_timeout = 5s;
    Server server(path, options);
    std::atomic<bool> handler_entered{false};
    std::atomic<bool> release_handler{false};
    server.on("hold", [&](const Request&) {
        handler_entered.store(true, std::memory_order_release);
        while (!release_handler.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return Response{200, "ok"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    const int send_buffer = 128 * 1024;
    expect(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0,
           "set pipeline sender buffer");
    const std::string body(256U * 1024U, 'b');
    std::string payload = "hold";
    payload += body;
    const auto request = frame(1, 1, 4, static_cast<std::uint32_t>(body.size()), payload.data(), payload.size());
    send_exact(fd, request.data(), request.size());
    const auto handler_deadline = std::chrono::steady_clock::now() + 1s;
    while (!handler_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= handler_deadline) {
            release_handler.store(true, std::memory_order_release);
            ::close(fd);
            server.stop();
            server_thread.join();
            cleanup_socket_artifacts(path);
            throw std::runtime_error("test failed: pipeline hold handler did not start");
        }
        std::this_thread::yield();
    }

    const int original_flags = ::fcntl(fd, F_GETFL, 0);
    expect(original_flags >= 0 && ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) == 0,
           "make pipeline sender nonblocking");
    std::size_t completed = 1;
    std::size_t offset = 0;
    std::size_t consecutive_stalls = 0;
    const auto send_deadline = std::chrono::steady_clock::now() + 750ms;
    while (completed < 128 && std::chrono::steady_clock::now() < send_deadline && consecutive_stalls < 5) {
        const ssize_t sent = send_no_signal(fd, request.data() + offset, request.size() - offset);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            consecutive_stalls = 0;
            if (offset == request.size()) {
                ++completed;
                offset = 0;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{fd, POLLOUT, 0};
            const int ready = ::poll(&descriptor, 1, 20);
            if (ready == 0) {
                ++consecutive_stalls;
            } else if (ready < 0 && errno != EINTR) {
                break;
            }
            continue;
        }
        break;
    }
    const bool bounded = completed < 128;

    release_handler.store(true, std::memory_order_release);
    ::close(fd);
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
    expect(bounded, "one pipelined peer must encounter bounded server-side input backpressure");
}

void test_pipelined_input_resumes_below_low_watermark() {
    using namespace easy_uds;

    const std::string path = socket_path("pipeline-resume");
    ServerOptions options;
    options.worker_threads = 1;
    options.io_timeout = 3s;
    options.request_timeout = 5s;
    Server server(path, options);
    std::atomic<bool> first_entered{false};
    std::atomic<bool> release_first{false};
    server.on("hold", [&](const Request&) {
        if (!first_entered.exchange(true, std::memory_order_acq_rel)) {
            while (!release_first.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return Response{200, "ok"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    const auto request = fixed_request(1, "hold");
    constexpr std::size_t request_count = 96;
    for (std::size_t index = 0; index < request_count; ++index) {
        send_exact(fd, request.data(), request.size());
    }
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!first_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            break;
        }
        std::this_thread::yield();
    }
    release_first.store(true, std::memory_order_release);

    timeval receive_timeout{};
    receive_timeout.tv_sec = 3;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
    bool all_responses = first_entered.load(std::memory_order_acquire);
    try {
        for (std::size_t index = 0; index < request_count; ++index) {
            std::array<unsigned char, 20> header{};
            std::array<char, 2> body{};
            recv_exact(fd, header.data(), header.size());
            recv_exact(fd, body.data(), body.size());
            if (header[5] != 2 || get_u32(header.data() + 12) != 200 ||
                std::string_view(body.data(), body.size()) != "ok") {
                all_responses = false;
                break;
            }
        }
    } catch (...) {
        all_responses = false;
    }

    ::close(fd);
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
    expect(all_responses, "paused pipelined input must resume and drain below the low-water mark");
}

} // namespace easy_uds::test
