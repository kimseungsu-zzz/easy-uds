#include "common.hpp"

namespace easy_uds::test {

void test_global_memory_budgets() {
    using namespace easy_uds;

    ServerOptions invalid;
    invalid.max_total_inflight_bytes = invalid.max_message_size - 1;
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-global-inflight"), invalid); },
        "global inflight budget below one message must be rejected");

    invalid = {};
    invalid.max_total_output_bytes = invalid.max_message_size - 1;
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-global-output"), invalid); },
        "global output budget below one message must be rejected");

    invalid = {};
    invalid.max_inflight_requests_per_connection = 0;
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-inflight-count"), invalid); },
        "per-connection in-flight request count must be positive");

    invalid = {};
    invalid.max_inflight_request_bytes_per_connection = invalid.max_message_size - 1;
    expect_throws<std::invalid_argument>(
        [&] { Server server(socket_path("bad-inflight-bytes"), invalid); },
        "per-connection in-flight byte budget must fit one message");

    const std::string path = socket_path("global-budget");
    ServerOptions options;
    options.worker_threads = 2;
    options.max_connections = 8;
    options.max_total_inflight_bytes = options.max_message_size;
    options.max_total_output_bytes = options.max_message_size + 20;
    options.max_inflight_requests_per_connection = 2;
    options.max_inflight_request_bytes_per_connection = options.max_message_size;
    options.io_timeout = 500ms;
    options.request_timeout = 2s;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);
    Client client(path);
    const Response response = client.request("ping");
    expect(response.status == 200 && response.body == "pong",
           "global memory budgets must preserve ordinary requests");
    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

} // namespace easy_uds::test
