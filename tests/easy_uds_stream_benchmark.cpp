#include "easy_uds/easy_uds.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

double mib_per_second(std::size_t bytes, Clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    return static_cast<double>(bytes) / (1024.0 * 1024.0) / seconds;
}

void wait_until_running(const easy_uds::Server& server) {
    while (!server.is_running()) {
        std::this_thread::yield();
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::size_t mebibytes = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 512U;
    const std::size_t chunk_size = argc > 2 ? static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)) : 65536U;
    const std::size_t byte_count = mebibytes * 1024U * 1024U;
    const std::string path = "/tmp/easy-uds-benchmark-" + std::to_string(static_cast<long long>(::getpid())) + ".sock";

    easy_uds::ServerOptions server_options;
    server_options.stream_chunk_size = chunk_size;
    server_options.max_stream_size = 0;
    server_options.io_timeout = std::chrono::seconds{30};
    server_options.stale_socket_grace_period = std::chrono::milliseconds{0};
    easy_uds::Server server(path, server_options);

    server.on_stream("upload", [](const easy_uds::StreamReader& body) {
        std::array<char, 256U * 1024U> buffer{};
        while (body(buffer.data(), buffer.size()) != 0) {
        }
        return easy_uds::StreamResponse{204, {}};
    });
    server.on_stream("download", [byte_count](const easy_uds::StreamReader&) {
        easy_uds::StreamReader body = [remaining = byte_count](char* buffer, std::size_t capacity) mutable {
            const std::size_t size = std::min(capacity, remaining);
            std::memset(buffer, 0x5a, size);
            remaining -= size;
            return size;
        };
        return easy_uds::StreamResponse{200, std::move(body)};
    });

    std::thread server_thread([&] { server.run(); });
    wait_until_running(server);

    easy_uds::ClientOptions client_options;
    client_options.stream_chunk_size = chunk_size;
    client_options.max_stream_size = 0;
    client_options.io_timeout = std::chrono::seconds{30};
    easy_uds::Client client(path, client_options);

    easy_uds::StreamReader upload = [remaining = byte_count](char* buffer, std::size_t capacity) mutable {
        const std::size_t size = std::min(capacity, remaining);
        std::memset(buffer, 0x5a, size);
        remaining -= size;
        return size;
    };
    const auto upload_start = Clock::now();
    (void)client.request_stream("upload", upload, {});
    const auto upload_elapsed = Clock::now() - upload_start;

    std::size_t received = 0;
    const auto download_start = Clock::now();
    (void)client.request_stream("download", {}, [&received](std::string_view chunk) { received += chunk.size(); });
    const auto download_elapsed = Clock::now() - download_start;

    server.stop();
    server_thread.join();
    (void)::unlink((path + ".lock").c_str());

    if (received != byte_count) {
        std::cerr << "incomplete download\n";
        return 1;
    }
    std::cout << "chunk=" << chunk_size << " bytes, payload=" << mebibytes << " MiB\n"
              << "upload:   " << mib_per_second(byte_count, upload_elapsed) << " MiB/s\n"
              << "download: " << mib_per_second(byte_count, download_elapsed) << " MiB/s\n";
    return 0;
}
