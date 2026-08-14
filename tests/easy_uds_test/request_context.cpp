#include "common.hpp"

#include <type_traits>

namespace easy_uds::test {
namespace {

void wait_for_flag(const std::atomic<bool>& flag,
                   std::chrono::milliseconds timeout,
                   const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::string("test failed: ") + message);
        }
        std::this_thread::yield();
    }
}

} // namespace

void test_request_context() {
    using namespace easy_uds;

    static_assert(!std::is_copy_constructible_v<RequestContext>);
    static_assert(!std::is_move_constructible_v<RequestContext>);

    const std::string path = socket_path("request-context");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 1s;
    options.request_timeout = 2s;
    Server server(path, options);

    std::atomic<bool> exact_metadata_ok{false};
    server.on(
        "context",
        RouteOptions{[&](const Request& request, const RequestContext& context) {
            const auto deadline = context.deadline();
            exact_metadata_ok.store(
                context.request_id() == request.request_id &&
                    context.peer().present &&
                    context.peer().pid == ::getpid() &&
                    context.arrival_time() <= RequestContext::Clock::now() &&
                    deadline.has_value() &&
                    *deadline > context.arrival_time() &&
                    !context.deadline_expired() &&
                    !context.connection_closing() &&
                    !context.server_stopping() && !context.stop_requested(),
                std::memory_order_release);
            return Response{200, "context"};
        }});
    server.on_prefix(
        "context.prefix.",
        RouteOptions{[](const Request&, const RequestContext& context) {
            return Response{context.deadline().has_value() ? 201 : 500,
                            "prefix"};
        }});
    server.on_serialized(
        "context.serialized",
        RouteOptions{[](const Request&, const RequestContext& context) {
            return Response{context.deadline().has_value() ? 202 : 500,
                            "serialized"};
        }});

    expect_throws<std::invalid_argument>(
        [&] {
            server.on("empty-context",
                      RouteOptions{RouteOptions::Handler{}});
        },
        "empty contextual handler should be rejected");
    expect_throws<std::runtime_error>(
        [&] {
            server.on("context",
                      RouteOptions{[](const Request&, const RequestContext&) {
                          return Response{};
                      }});
        },
        "contextual route should share duplicate detection with simple routes");

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    Session session = client.session();
    expect(session.request("context").body == "context",
           "contextual exact route should respond");
    expect(exact_metadata_ok.load(std::memory_order_acquire),
           "request context should expose request, peer, arrival, and deadline metadata");
    expect(client.request("context.prefix.child").status == 201,
           "contextual prefix route should respond");
    expect(client.request("context.serialized").status == 202,
           "contextual serialized route should respond");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);

    const std::string no_deadline_path = socket_path("context-no-deadline");
    ServerOptions no_deadline_options;
    no_deadline_options.request_timeout = 0ms;
    Server no_deadline_server(no_deadline_path, no_deadline_options);
    std::atomic<bool> stop_handler_entered{false};
    std::atomic<bool> server_stop_observed{false};
    no_deadline_server.on(
        "context",
        RouteOptions{[](const Request&, const RequestContext& context) {
            return Response{context.deadline().has_value() ? 500 : 200, "none"};
        }});
    no_deadline_server.on(
        "wait-for-stop",
        RouteOptions{[&](const Request&, const RequestContext& context) {
            stop_handler_entered.store(true, std::memory_order_release);
            const auto fallback = std::chrono::steady_clock::now() + 1s;
            while (!context.stop_requested() &&
                   std::chrono::steady_clock::now() < fallback) {
                std::this_thread::yield();
            }
            server_stop_observed.store(
                context.server_stopping() && context.stop_requested(),
                std::memory_order_release);
            return Response{200, "stopped"};
        }});
    std::thread no_deadline_thread([&] { no_deadline_server.run(); });
    wait_until_running(no_deadline_server);
    expect(Client(no_deadline_path).request("context").status == 200,
           "disabled request timeout should produce no context deadline");
    std::thread stop_client([&] {
        try {
            (void)Client(no_deadline_path).request("wait-for-stop");
        } catch (const std::system_error&) {
        }
    });
    wait_for_flag(stop_handler_entered, 1s,
                  "server-stop context handler did not start");
    no_deadline_server.stop();
    stop_client.join();
    no_deadline_thread.join();
    expect(server_stop_observed.load(std::memory_order_acquire),
           "handler should observe server shutdown as a cooperative stop");
    cleanup_socket_artifacts(no_deadline_path);

    const std::string deadline_path = socket_path("context-deadline");
    ServerOptions deadline_options;
    deadline_options.worker_threads = 1;
    deadline_options.io_timeout = 1s;
    deadline_options.request_timeout = 80ms;
    Server deadline_server(deadline_path, deadline_options);
    std::atomic<bool> deadline_stop_observed{false};
    deadline_server.on(
        "expire",
        RouteOptions{[&](const Request&, const RequestContext& context) {
            const auto fallback = std::chrono::steady_clock::now() + 1s;
            while (!context.stop_requested() &&
                   std::chrono::steady_clock::now() < fallback) {
                std::this_thread::yield();
            }
            deadline_stop_observed.store(
                context.deadline_expired() && context.stop_requested(),
                std::memory_order_release);
            return Response{200, "expired"};
        }});
    std::thread deadline_thread([&] { deadline_server.run(); });
    wait_until_running(deadline_server);
    try {
        (void)Client(deadline_path).request("expire");
    } catch (const std::system_error&) {
    }
    wait_for_flag(deadline_stop_observed, 1s,
                  "handler did not observe its expired deadline");
    deadline_server.stop();
    deadline_thread.join();
    cleanup_socket_artifacts(deadline_path);

    const std::string cancel_path = socket_path("context-cancel");
    ServerOptions cancel_options;
    cancel_options.worker_threads = 1;
    cancel_options.request_timeout = 2s;
    cancel_options.io_timeout = 1s;
    Server cancel_server(cancel_path, cancel_options);
    std::atomic<bool> handler_entered{false};
    std::atomic<bool> connection_stop_observed{false};
    cancel_server.on(
        "wait",
        RouteOptions{[&](const Request&, const RequestContext& context) {
            handler_entered.store(true, std::memory_order_release);
            const auto fallback = std::chrono::steady_clock::now() + 1500ms;
            while (!context.stop_requested() &&
                   std::chrono::steady_clock::now() < fallback) {
                std::this_thread::yield();
            }
            connection_stop_observed.store(
                context.connection_closing() && context.stop_requested(),
                std::memory_order_release);
            return Response{200, "stopped"};
        }});
    std::thread cancel_server_thread([&] { cancel_server.run(); });
    wait_until_running(cancel_server);

    const int fd = connect_raw(cancel_path);
    const auto request = fixed_request(9, "wait");
    send_exact(fd, request.data(), request.size());
    wait_for_flag(handler_entered, 1s, "context cancellation handler did not start");
    ::close(fd);
    wait_for_flag(connection_stop_observed, 1s,
                  "handler did not observe disconnected connection");

    cancel_server.stop();
    cancel_server_thread.join();
    cleanup_socket_artifacts(cancel_path);
}

} // namespace easy_uds::test
