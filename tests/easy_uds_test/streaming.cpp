#include "common.hpp"

namespace easy_uds::test {

unsigned char pattern_byte(std::size_t index) {
    return static_cast<unsigned char>(index * 31U + 7U);
}

void test_streams() {
    using namespace easy_uds;

    constexpr std::size_t upload_size = 12U * 1024U * 1024U + 137U;
    constexpr std::size_t download_size = 10U * 1024U * 1024U + 91U;
    const std::string path = socket_path("stream");

    ServerOptions server_options;
    server_options.worker_threads = 2;
    server_options.max_connections = 8;
    server_options.stream_chunk_size = 4093;
    server_options.max_stream_size = 32U * 1024U * 1024U;
    server_options.io_timeout = 2s;
    server_options.request_timeout = 5s;
    server_options.stale_socket_grace_period = 0ms;

    Server server(path, server_options);
    server.on_stream("transfer", [](const StreamReader& request_body, const Request&) {
        std::array<char, 3331> buffer{};
        std::size_t received = 0;
        while (true) {
            const std::size_t size = request_body(buffer.data(), buffer.size());
            if (size == 0) {
                break;
            }
            for (std::size_t index = 0; index < size; ++index) {
                if (static_cast<unsigned char>(buffer[index]) != pattern_byte(received + index)) {
                    throw std::runtime_error("upload pattern mismatch");
                }
            }
            received += size;
        }
        if (received != upload_size) {
            throw std::runtime_error("upload length mismatch");
        }

        StreamReader response = [offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
            const std::size_t remaining = download_size - offset;
            const std::size_t size = std::min(capacity, remaining);
            for (std::size_t index = 0; index < size; ++index) {
                output[index] = pattern_byte(offset + index);
            }
            offset += size;
            return size;
        };
        return StreamResponse{206, std::move(response)};
    });
    server.on_stream("prefix-only", [](const StreamReader& request_body, const Request&) {
        std::array<char, 8> prefix{};
        (void)request_body(prefix.data(), prefix.size());
        return StreamResponse{204, {}};
    });
    server.on_stream("throws-stream", [](const StreamReader&, const Request&) -> StreamResponse {
        throw std::runtime_error("stream failure");
    });
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream_prefix("dl.", [](const StreamReader&, const Request& request) {
        const std::string body = "data:" + request.route;
        return StreamResponse{200, [body, offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
                                   const std::size_t take = std::min(capacity, body.size() - offset);
                                   if (take == 0) {
                                       return std::size_t{0};
                                   }
                                   std::memcpy(output, body.data() + offset, take);
                                   offset += take;
                                   return take;
                               }};
    });
    expect_throws<std::runtime_error>(
        [&] { server.on_stream("transfer", [](const StreamReader&, const Request&) { return StreamResponse{}; }); },
        "duplicate streaming route should be rejected");

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.stream_chunk_size = 8191;
    client_options.max_stream_size = 32U * 1024U * 1024U;
    client_options.connect_timeout = 500ms;
    client_options.io_timeout = 2s;
    client_options.request_timeout = 5s;
    Client client(path, client_options);

    StreamReader upload = [offset = std::size_t{0}](char* output, std::size_t capacity) mutable {
        const std::size_t remaining = upload_size - offset;
        const std::size_t size = std::min(capacity, remaining);
        for (std::size_t index = 0; index < size; ++index) {
            output[index] = pattern_byte(offset + index);
        }
        offset += size;
        return size;
    };

    std::size_t response_offset = 0;
    const Status status = client.request_stream("transfer", upload, [&](std::string_view chunk) {
        for (std::size_t index = 0; index < chunk.size(); ++index) {
            expect(static_cast<unsigned char>(chunk[index]) == pattern_byte(response_offset + index),
                   "download pattern should be preserved");
        }
        response_offset += chunk.size();
    });
    expect(status == 206, "stream status should round-trip");
    expect(response_offset == download_size, "large response should be delivered completely");

    std::size_t remaining = 256U * 1024U;
    StreamReader ignored_tail = [&remaining](char* output, std::size_t capacity) {
        const std::size_t size = std::min(capacity, remaining);
        std::memset(output, 'x', size);
        remaining -= size;
        return size;
    };
    expect(client.request_stream("prefix-only", ignored_tail, {}) == 204,
           "server should drain an unread request tail before responding");

    StreamReader missing_body = [](char*, std::size_t) { return std::size_t{0}; };
    expect(client.request_stream("missing-stream", missing_body, {}) == 404,
           "missing stream route should return 404");
    expect(client.request_stream("throws-stream", missing_body, {}) == 500,
           "throwing stream handler should return 500");

    std::string prefix_body;
    expect(client.request_stream("dl.sensor", missing_body, [&](std::string_view chunk) {
               prefix_body.append(chunk.data(), chunk.size());
           }) == 200,
           "prefix stream route should match");
    expect(prefix_body == "data:dl.sensor", "prefix stream body");

    // Session streams run on their own dedicated connection.
    Session session = client.session();
    std::string session_body;
    expect(session.request_stream("dl.robot", missing_body, [&](std::string_view chunk) {
               session_body.append(chunk.data(), chunk.size());
           }) == 200,
           "session stream should work");
    expect(session_body == "data:dl.robot", "session stream body");
    expect(session.request("ping").body == "pong", "session should stay usable after a stream");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_limit_reserves_worker() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-limit");
    ServerOptions options;
    options.worker_threads = 3;  // automatic max_concurrent_streams = 2
    options.max_connections = 16;
    options.io_timeout = 500ms;
    options.request_timeout = 5s;
    Server server(path, options);
    server.on("ping", [](const Request&) { return Response{200, "pong"}; });
    server.on_stream("block", [](const StreamReader& body, const Request&) {
        std::array<char, 1024> buffer{};
        while (true) {
            const std::size_t size = body(buffer.data(), buffer.size());
            if (size == 0) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        return StreamResponse{200, {}};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    Client client(path);
    // Two long-lived streams take both stream slots.
    const auto make_slow_reader = [] {
        return StreamReader{[r = std::size_t{4 * 1024 * 1024}](char*, std::size_t c) mutable {
            const std::size_t take = std::min(c, r);
            if (take == 0) return std::size_t{0};
            r -= take;
            return take;
        }};
    };
    StreamReader slow_a = make_slow_reader();
    StreamReader slow_b = make_slow_reader();
    const auto stream_start = std::chrono::steady_clock::now();
    std::thread stream_a([&] { (void)client.request_stream("block", slow_a, {}); });
    std::thread stream_b([&] { (void)client.request_stream("block", slow_b, {}); });
    while (std::chrono::steady_clock::now() - stream_start < 100ms) {
        std::this_thread::yield();
    }

    // The third stream exceeds the slot limit and is rejected by closing.
    StreamReader third = [r = std::size_t{1024 * 1024}](char*, std::size_t c) mutable {
        const std::size_t take = std::min(c, r);
        if (take == 0) return std::size_t{0};
        r -= take;
        return take;
    };
    expect_throws<std::system_error>([&] { (void)client.request_stream("block", third, {}); },
                                     "excess stream should be rejected");

    // The reserved worker still serves regular RPC.
    expect(client.request("ping").body == "pong", "fixed RPC must stay available during stream saturation");

    stream_a.join();
    stream_b.join();

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

void test_stream_timeout_is_independent() {
    using namespace easy_uds;

    const std::string path = socket_path("stream-timeout-independent");
    ServerOptions options;
    options.worker_threads = 2;
    options.io_timeout = 500ms;
    options.request_timeout = 50ms;
    options.stream_timeout = 500ms;
    Server server(path, options);
    server.on_stream("slow-stream", [](const StreamReader& body, const Request&) {
        std::array<char, 8> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        std::this_thread::sleep_for(120ms);
        return StreamResponse{200, [sent = false](char* output, std::size_t capacity) mutable {
                                  if (sent || capacity == 0) {
                                      return std::size_t{0};
                                  }
                                  output[0] = 'x';
                                  sent = true;
                                  return std::size_t{1};
                              }};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    ClientOptions client_options;
    client_options.io_timeout = 500ms;
    client_options.stream_timeout = 500ms;
    std::string response_body;
    const Status status = Client(path, client_options).request_stream(
        "slow-stream", [](char*, std::size_t) { return std::size_t{0}; },
        [&](std::string_view chunk) { response_body.append(chunk.data(), chunk.size()); });
    expect(status == 200 && response_body == "x",
           "stream_timeout must be independent from the shorter regular request_timeout");

    server.stop();
    server_thread.join();
    cleanup_socket_artifacts(path);
}

} // namespace easy_uds::test
