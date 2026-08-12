#include "common.hpp"

namespace easy_uds::test {

void test_multiplexed_session() {
    using namespace easy_uds;

    const std::string path = socket_path("mux");
    ServerOptions options;
    options.worker_threads = 4;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    options.session_idle_grace = 0ms;  // exercise pure reactor rearm with pipelined read-ahead
    Server server(path, options);
    std::atomic<bool> slow_entered{false};
    server.on("echo", [](const Request& request) { return Response{200, request.body}; });
    server.on("slow", [&](const Request& request) {
        if (request.body == "slow") {
            slow_entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(250ms);
        }
        return Response{200, request.body};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 5s;
    Client client(path, client_options);

    Session session = client.session();
    expect(session.request("echo", "first").body == "first", "session first request");

    // Prove that a later fast request is dispatched while a slow handler on
    // this same connection is still running.
    std::exception_ptr explicit_slow_error;
    std::thread explicit_slow([&] {
        try {
            expect(session.request("slow", "slow").body == "slow", "explicit slow response");
        } catch (...) {
            explicit_slow_error = std::current_exception();
        }
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + 1s;
    while (!slow_entered.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= entered_deadline) {
            explicit_slow.join();
            throw std::runtime_error("test failed: slow multiplexed handler did not start");
        }
        std::this_thread::yield();
    }
    const auto fast_started = std::chrono::steady_clock::now();
    expect(session.request("slow", "fast").body == "fast", "fast multiplexed response");
    const auto fast_elapsed = std::chrono::steady_clock::now() - fast_started;
    explicit_slow.join();
    if (explicit_slow_error) {
        std::rethrow_exception(explicit_slow_error);
    }
    expect(fast_elapsed < 200ms, "slow request must not head-of-line block a later fast request");

    // Concurrent multiplexed requests; the slow one must not block the others
    // even though it was issued first.
    std::atomic<std::size_t> completed{0};
    std::vector<std::exception_ptr> errors(8);
    std::vector<std::thread> callers;
    for (std::size_t i = 0; i < errors.size(); ++i) {
        callers.emplace_back([&, i] {
            try {
                const std::string body = i == 0 ? "slow" : "m" + std::to_string(i);
                const Response response = session.request("slow", body);
                expect(response.status == 200 && response.body == body, "multiplexed echo round-trip");
                completed.fetch_add(1, std::memory_order_release);
            } catch (...) {
                errors[i] = std::current_exception();
            }
        });
    }
    for (auto& caller : callers) {
        caller.join();
    }
    expect(completed.load(std::memory_order_acquire) == errors.size(), "all multiplexed callers should succeed");
    for (const auto& error : errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    // Repeatedly publish responses to distinct waiters on one shared session.
    // This catches missed targeted notifications and stale in-flight slot
    // pointers that a single request per caller would rarely expose.
    constexpr std::size_t repeated_callers = 8;
    constexpr std::size_t requests_per_caller = 64;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<std::exception_ptr> repeated_errors(repeated_callers);
    callers.clear();
    for (std::size_t caller_index = 0; caller_index < repeated_callers; ++caller_index) {
        callers.emplace_back([&, caller_index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            try {
                for (std::size_t request_index = 0; request_index < requests_per_caller; ++request_index) {
                    const std::string body = std::to_string(caller_index) + ":" + std::to_string(request_index);
                    const Response response = session.request("echo", body);
                    expect(response.status == 200 && response.body == body, "repeated multiplexed round-trip");
                }
            } catch (...) {
                repeated_errors[caller_index] = std::current_exception();
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != repeated_callers) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& caller : callers) {
        caller.join();
    }
    for (const auto& error : repeated_errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }

    // Raw pipelining: two requests sent back-to-back on one connection are
    // both answered (reactor parses sequentially, multiplex id correlates).
    const int raw_fd = connect_raw(path);
    const std::vector<unsigned char> r1 = fixed_request(7, "echo");
    const std::vector<unsigned char> r2 = fixed_request(8, "echo");
    std::vector<unsigned char> pipeline(r1);
    pipeline.insert(pipeline.end(), r2.begin(), r2.end());
    expect(send_no_signal(raw_fd, pipeline.data(), pipeline.size()) == static_cast<ssize_t>(pipeline.size()),
           "pipelined frames should be written");

    std::array<unsigned char, 20> header{};
    ssize_t got = ::recv(raw_fd, header.data(), header.size(), 0);
    expect(got == static_cast<ssize_t>(header.size()), "first pipelined response header should arrive");
    got = ::recv(raw_fd, header.data(), header.size(), 0);
    expect(got == static_cast<ssize_t>(header.size()), "second pipelined response header should arrive");
    ::close(raw_fd);

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_fragmented_fast_path_header() {
    using namespace easy_uds;

    const std::string path = socket_path("fragmented-fast-path");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 1s;
    options.session_idle_grace = 1ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    const auto first = fixed_request(1, "ping");
    expect(send_no_signal(fd, first.data(), first.size()) == static_cast<ssize_t>(first.size()),
           "first raw request should be written");
    std::array<unsigned char, 20> response_header{};
    recv_exact(fd, response_header.data(), response_header.size());
    std::array<char, 4> response_body{};
    recv_exact(fd, response_body.data(), response_body.size());
    expect(std::string(response_body.data(), response_body.size()) == "pong", "first raw response body");

    const auto second = fixed_request(2, "ping");
    expect(send_no_signal(fd, second.data(), 10) == 10, "partial follow-up header should be written");
    std::this_thread::sleep_for(20ms);  // longer than session_idle_grace
    expect(send_no_signal(fd, second.data() + 10, second.size() - 10) ==
               static_cast<ssize_t>(second.size() - 10),
           "remaining follow-up frame should be written");
    recv_exact(fd, response_header.data(), response_header.size());
    expect(get_u32(response_header.data() + 8) == 2, "fragmented follow-up response id");
    recv_exact(fd, response_body.data(), response_body.size());
    expect(std::string(response_body.data(), response_body.size()) == "pong",
           "fragmented follow-up should complete");
    ::close(fd);

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_connection_reuse() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-reuse");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream("discard", [](const StreamReader& body, const Request&) {
        std::array<char, 64> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        return StreamResponse{204, {}};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int fd = connect_raw(path);
    auto stream_start = frame(3, 11, 7, 0, "discard", 7);
    const auto stream_end = frame(5, 11, 0, 0, nullptr, 0);
    stream_start.insert(stream_start.end(), stream_end.begin(), stream_end.end());
    expect(send_no_signal(fd, stream_start.data(), stream_start.size()) ==
               static_cast<ssize_t>(stream_start.size()),
           "stream request should be written");

    std::array<unsigned char, 20> header{};
    recv_exact(fd, header.data(), header.size());
    expect(header[5] == 6 && get_u32(header.data() + 8) == 11, "stream response start");
    recv_exact(fd, header.data(), header.size());
    expect(header[5] == 8 && get_u32(header.data() + 8) == 11, "stream response end");

    const auto fixed = fixed_request(12, "ping");
    expect(send_no_signal(fd, fixed.data(), fixed.size()) == static_cast<ssize_t>(fixed.size()),
           "fixed request after stream should be written");
    recv_exact(fd, header.data(), header.size());
    expect(header[5] == 2 && get_u32(header.data() + 8) == 12, "fixed response after stream");
    std::array<char, 4> fixed_body{};
    recv_exact(fd, fixed_body.data(), fixed_body.size());
    expect(std::string(fixed_body.data(), fixed_body.size()) == "pong", "fixed response body after stream");

    // The fixed-request continuation lease must replay a following stream
    // header to the reactor instead of rejecting it as a fixed frame.
    try {
        const auto second_stream_start = frame(3, 13, 4, 0, "sink", 4);
        const auto second_stream_end = frame(5, 13, 0, 0, nullptr, 0);
        send_exact(fd, second_stream_start.data(), second_stream_start.size());
        send_exact(fd, second_stream_end.data(), second_stream_end.size());
        recv_exact(fd, header.data(), header.size());
        expect(header[5] == 6 && get_u32(header.data() + 8) == 13,
               "stream response after fixed request (type=" + std::to_string(header[5]) +
                   ", id=" + std::to_string(get_u32(header.data() + 8)) + ")");
        recv_exact(fd, header.data(), header.size());
        expect(header[5] == 8 && get_u32(header.data() + 8) == 13, "stream end after fixed request");
    } catch (...) {
        ::close(fd);
        server.stop();
        server_thread.join();
        cleanup_socket_artifacts(path);
        throw;
    }
    ::close(fd);

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_client_stream_response_limit() {
    using namespace easy_uds;

    const std::string path = socket_path("client-stream-limit");
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::system_error(errno, std::generic_category(), "fake stream listener failed");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
        const int error = errno;
        ::close(listener);
        cleanup_socket_artifacts(path);
        throw std::system_error(error, std::generic_category(), "fake stream server setup failed");
    }

    std::thread fake_server([&] {
        const int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) {
            return;
        }
        std::array<unsigned char, 41> request{};
        recv_exact(fd, request.data(), request.size());
        const auto start = frame(6, 0, 200, 0, nullptr, 0);
        const auto first = frame(7, 0, 3, 0, "abc", 3);
        const auto second = frame(7, 0, 3, 0, "def", 3);
        const auto end = frame(8, 0, 0, 0, nullptr, 0);
        send_exact(fd, start.data(), start.size());
        send_exact(fd, first.data(), first.size());
        send_exact(fd, second.data(), second.size());
        send_exact(fd, end.data(), end.size());
        ::close(fd);
    });

    ClientOptions options;
    options.max_stream_size = 4;
    options.io_timeout = 1s;
    options.stream_timeout = 1s;
    expect_throws<std::length_error>(
        [&] {
            (void)Client(path, options).request_stream("x", [](char*, std::size_t) { return std::size_t{0}; },
                                                       [](std::string_view) {});
        },
        "client must enforce max_stream_size on the response body");
    fake_server.join();
    ::close(listener);
    cleanup_socket_artifacts(path);
}

void test_client_rejects_mismatched_response_ids() {
    using namespace easy_uds;

    const std::string path = socket_path("client-response-id");
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        throw std::system_error(errno, std::generic_category(), "fake response-id listener failed");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 2) != 0) {
        const int error = errno;
        ::close(listener);
        cleanup_socket_artifacts(path);
        throw std::system_error(error, std::generic_category(), "fake response-id server setup failed");
    }

    std::atomic<bool> release_session_peer{false};
    std::thread fake_server([&] {
        for (int connection = 0; connection < 2; ++connection) {
            const int fd = ::accept(listener, nullptr, nullptr);
            if (fd < 0) {
                return;
            }
            std::array<unsigned char, 20> request_header{};
            recv_exact(fd, request_header.data(), request_header.size());
            const std::size_t payload_size =
                static_cast<std::size_t>(get_u32(request_header.data() + 12)) +
                get_u32(request_header.data() + 16);
            std::vector<unsigned char> payload(payload_size);
            recv_exact(fd, payload.data(), payload.size());

            const std::uint32_t request_id = get_u32(request_header.data() + 8);
            const std::uint32_t wrong_id = request_id == std::numeric_limits<std::uint32_t>::max()
                                               ? request_id - 1
                                               : request_id + 1;
            const auto response = frame(2, wrong_id, 200, 4, "pong", 4);
            send_exact(fd, response.data(), response.size());
            if (connection == 1) {
                const auto deadline = std::chrono::steady_clock::now() + 1s;
                while (!release_session_peer.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                }
            }
            ::close(fd);
        }
    });

    ClientOptions options;
    options.io_timeout = 1s;
    options.request_timeout = 500ms;
    expect_throws<std::runtime_error>([&] { (void)Client(path, options).request("ping"); },
                                      "one-shot client must reject a mismatched response id");

    Session session = Client(path, options).session();
    bool rejected_as_protocol_error = false;
    try {
        (void)session.request("ping");
    } catch (const std::system_error&) {
        // A timeout means the unknown id was silently ignored.
    } catch (const std::runtime_error&) {
        rejected_as_protocol_error = true;
    }
    release_session_peer.store(true, std::memory_order_release);
    fake_server.join();
    ::close(listener);
    cleanup_socket_artifacts(path);

    expect(rejected_as_protocol_error, "session must fail immediately on an unknown response id");
    expect_throws<std::logic_error>([&] { (void)session.request("ping"); },
                                    "unknown response id must permanently break the session");
}
void test_session_broken_after_shutdown() {
    using namespace easy_uds;

    const std::string path = socket_path("session-broken");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    Session session = client.session();
    expect(session.request("ping").body == "pong", "session works before shutdown");

    server.stop();
    server_thread.join();

    // The reader thread may observe the close before this call runs, so the
    // first failure is either an I/O system_error or the sticky logic_error;
    // both mark the session unusable.
    expect_throws<std::exception>([&] { (void)session.request("ping"); },
                                  "session request against a stopped server should fail");
    expect_throws<std::logic_error>([&] { (void)session.request("ping"); },
                                    "broken session should reject later requests");
    cleanup_socket_artifacts(path);
}

void test_session_broken_after_timeout() {
    using namespace easy_uds;

    const std::string path = socket_path("session-timeout");
    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.io_timeout = 500ms;
    Server server(path, server_options);
    server.on("slow", [](const Request&) {
        std::this_thread::sleep_for(100ms);
        return Response{200, "late"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 20ms;
    Session session = Client(path, client_options).session();
    expect_throws<std::system_error>([&] { (void)session.request("slow"); },
                                     "session request should report its deadline");
    expect_throws<std::logic_error>([&] { (void)session.request("slow"); },
                                    "timed-out session should remain permanently unusable");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_idle_session_survives_io_timeout() {
    using namespace easy_uds;

    const std::string path = socket_path("session-idle");
    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.io_timeout = 0ms;
    Server server(path, server_options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 50ms;
    client_options.request_timeout = 1s;
    Session session = Client(path, client_options).session();
    expect(session.request("ping").body == "pong", "session works before idle period");
    std::this_thread::sleep_for(150ms);
    expect(session.request("ping").body == "pong", "idle session must outlive client io_timeout");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_session_move() {
    using namespace easy_uds;

    const std::string path = socket_path("session-move");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    Session session = client.session();
    expect(session.request("ping").body == "pong", "session works before being moved");

    Session moved = std::move(session);
    expect(moved.request("ping").body == "pong", "moved session keeps the connection");
    expect_throws<std::logic_error>([&] { (void)session.request("ping"); },
                                    "moved-from session must reject requests");

    Session destination = client.session();
    expect(destination.request("ping").body == "pong", "move destination is active before assignment");
    destination = std::move(moved);
    expect(destination.request("ping").body == "pong", "move-assigned session keeps the source connection");
    expect_throws<std::logic_error>([&] { (void)moved.request("ping"); },
                                    "move-assigned source must reject requests");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_reactor_request_timeouts() {
    using namespace easy_uds;

    const std::string path = socket_path("reactor-timeouts");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 200ms;
    options.request_timeout = 500ms;
    options.session_idle_grace = 100ms;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on("slow-normal", [](const Request&) {
        std::this_thread::sleep_for(300ms);
        return Response{200, "normal-done"};
    });
    server.on_serialized("slow", [](const Request&) {
        std::this_thread::sleep_for(300ms);
        return Response{200, "done"};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    const int idle_fd = connect_raw(path);
    expect_peer_closed(idle_fd, 1s, "idle reactor connection should honor io_timeout");
    ::close(idle_fd);

    const int header_fd = connect_raw(path);
    const unsigned char first_byte = 'E';
    expect(send_no_signal(header_fd, &first_byte, 1) == 1, "partial header byte should be written");
    expect_peer_closed(header_fd, 1s, "partial header should honor io_timeout");
    ::close(header_fd);

    const int payload_fd = connect_raw(path);
    const auto incomplete = frame(1, 41, 8, 0, nullptr, 0);
    expect(send_no_signal(payload_fd, incomplete.data(), incomplete.size()) ==
               static_cast<ssize_t>(incomplete.size()), "incomplete request header should be written");
    for (const char byte : std::string{"ping"}) {
        std::this_thread::sleep_for(100ms);
        expect(send_no_signal(payload_fd, &byte, 1) == 1, "payload progress byte should be written");
    }
    expect_peer_closed(payload_fd, 1s, "partial payload should honor request_timeout");
    ::close(payload_fd);

    // A serialized handler owns a complete request while it runs. Its socket
    // must not be mistaken for an idle, incomplete reactor request.
    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.request_timeout = 500ms;
    const Response normal_response = Client(path, client_options).request("slow-normal");
    expect(normal_response.status == 200 && normal_response.body == "normal-done",
           "normal handler should survive a shorter reactor io_timeout");
    Session serialized_session = Client(path, client_options).session();
    expect(serialized_session.request("ping").body == "pong", "session fast path should be established");
    const Response response = serialized_session.request("slow");
    expect(response.status == 200 && response.body == "done",
           "serialized handler should survive a shorter reactor io_timeout");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

} // namespace easy_uds::test
